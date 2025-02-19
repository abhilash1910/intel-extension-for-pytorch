#include "graph_fuser.h"
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/common_subexpression_elimination.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>
#include "graph_helper.h"

namespace torch {
namespace jit {
namespace fuser {
namespace onednn {

namespace {

struct WorkBlock : public std::pair<Node*, Node*> {
  using pair::pair;

  Node* begin() {
    return this->first;
  }
  Node* end() {
    return this->second;
  }
};

class GraphRewriter {
 public:
  GraphRewriter(Block* block, std::shared_ptr<Graph> graph, AliasDb& aliasDb)
      : block_(block),
        graph_(std::move(graph)),
        aliasDb_(aliasDb),
        llgaHelper(graph_) {}

  void run() {
    // We maintain alias db correctness in-place while building up the autodiff
    // subgraphs, however it is difficult to preserve correctness when
    // un-inlining autodiff subgraphs. We first recursively construct all
    // subgraphs and then recursively cleanup & unmerge the small subgraphs
    buildupSubgraphs();
    cleanupSubgraphs();

    // Run CSE globally once to eliminate duplicates that may have occurred
    // while inlining subgraphs.
    EliminateCommonSubexpression(graph_);
    EliminateDeadCode(graph_);
  }

  void cleanupSubgraphs() {
    auto curNode = *block_->nodes().rbegin();
    while (curNode != *block_->nodes().rend()) {
      // Save the previous node, since we might delete `curNode` in next block
      auto prevNode = curNode->prev();
      if (llgaHelper.isLlgaSubgraph(curNode)) {
        // Unmerge subgraph if we don't get every nodes of a partition
        // into the subgraph due to failed alias check
        llgaHelper.unmergeIfAnyNodeIsMissing(curNode);
      }
      curNode = prevNode;
    }

    for (Node* n : block_->nodes()) {
      for (Block* b : n->blocks()) {
        GraphRewriter(b, graph_, aliasDb_).cleanupSubgraphs();
      }
    }
  }

  void buildupSubgraphs() {
    // We need to run the rewriter multiple times in order to get all merge
    // opportunities. This is because moveBeforeTopologicalValid may reorder
    // nodes to be AFTER the current iteration point. In order to properly
    // consider those nodes for merging, we need run the pass until no changes
    // have been made.
    //
    // Example:
    //   c = f(a, b)
    //   d = f(c)
    //   e = f(d)  <- iter is here, moving upward
    // After c.moveBeforeTopologicallyValid(e), we have:
    //   c = f(a, b)
    //   e = f(d)  <- iter still here
    //   d = f(c)  <- this was node moved on the other side.

    // see [workblocks]
    auto workblocks = buildWorkBlocks();
    for (auto& workblock : workblocks) {
      bool any_changed = true;
      while (any_changed) {
        any_changed = false;
        auto workblock_end = workblock.end()->reverseIterator();
        auto workblock_begin = workblock.begin()->reverseIterator();
        for (auto it = workblock_end; it != workblock_begin;) {
          bool changed;
          std::tie(it, changed) = scanNode(*it, workblock_begin);
          any_changed |= changed;
        }
      }
    }

    // Construct Subgraphs Recursively
    for (Node* n : block_->nodes()) {
      for (auto subBlock : n->blocks()) {
        GraphRewriter(subBlock, graph_, aliasDb_).buildupSubgraphs();
      }
    }
  }

 private:
  std::vector<WorkBlock> buildWorkBlocks() {
    // [workblocks]
    // the IR has many nodes which can never be reordered around, such as a
    // prim::Bailout. if a node N is surrounded by two nodes which cannot be
    // reordered, A and B, then a fusion group that is created from N
    // can only contain nodes from (A, B) The nodes from A to B represent one
    // work block for the subgraph rewriter to work on. By creating these up
    // front, we avoid retraversing the whole graph block any time scanNode
    // returns

    Node* end_bound_node = block_->return_node();
    Node* curr = end_bound_node->prev();

    std::vector<WorkBlock> worklist;

    while (curr != block_->param_node()) {
      // cannot reorder around side effectful nodes
      if (curr->hasSideEffects()) {
        worklist.emplace_back(curr, end_bound_node);
        end_bound_node = curr;
      }
      curr = curr->prev();
    }

    worklist.emplace_back(curr, end_bound_node);

    return worklist;
  }

  std::pair<graph_node_list::iterator, bool> scanNode(
      Node* consumer,
      graph_node_list::iterator workblock_begin) {
    GRAPH_DEBUG("Scanning ", consumer->kind().toQualString());

    if (llgaHelper.shouldConsiderForMerge(consumer)) {
      if (!llgaHelper.isLlgaSubgraph(consumer)) {
        consumer = llgaHelper.createSingletonSubgraph(consumer, aliasDb_);
      }

      // Iterate through the workblock to merge nodes of the
      // same partition determined by LLGA graph helper.
      // Nodes like B and C do not share a common input but belong to a
      // same partition, and thus we cannot only scan the input nodes
      // to find merging opportunities. Instead, we have to scan through
      // the whole workblock, which might lead to O^2 accesses in worst case
      //              A
      //      + - - / - \ - - +
      //      |    B     C    |
      //      |    |     |    |
      //      |    D     E    |
      //      + - - \ - / - - +
      //              F
      auto prev = ++consumer->reverseIterator();
      for (auto it = prev; it != workblock_begin; it++) {
        if (auto group = tryMerge(consumer, *it)) {
          // we successfully merged, so the new group's `inputs` may have
          // changed. So rescan the new group for more merging opportunities.
          return std::make_pair(group.value()->reverseIterator(), true);
        }
      }
    }

    return std::make_pair(++consumer->reverseIterator(), false);
  }

  // Try to merge `producer` into `consumer`. If successful, this destroys
  // `producer` and returns the `consumer` group.
  c10::optional<Node*> tryMerge(Node* consumer, Node* producer) {
    AT_ASSERT(llgaHelper.isLlgaSubgraph(consumer));
    bool canMerge = llgaHelper.shouldMerge(producer, consumer) &&
        aliasDb_.moveBeforeTopologicallyValid(producer, consumer);

    if (!canMerge) {
      return c10::nullopt;
    }

    llgaHelper.mergeNodeIntoSubgraph(producer, consumer, aliasDb_);

    return consumer;
  }

  Block* block_;
  std::shared_ptr<Graph> graph_;
  AliasDb& aliasDb_;
  LlgaGraphHelper llgaHelper;
};
} // anonymous namespace

void CreateLlgaSubgraphs(std::shared_ptr<Graph>& graph) {
  AliasDb db(graph);
  GraphRewriter(graph->block(), graph, db).run();
}

} // namespace onednn
} // namespace fuser
} // namespace jit
} // namespace torch