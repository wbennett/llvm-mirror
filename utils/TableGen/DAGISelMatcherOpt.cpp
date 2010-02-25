//===- DAGISelMatcherOpt.cpp - Optimize a DAG Matcher ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the DAG Matcher optimizer.
//
//===----------------------------------------------------------------------===//

#include "DAGISelMatcher.h"
using namespace llvm;

static void ContractNodes(OwningPtr<MatcherNode> &MatcherPtr) {
  // If we reached the end of the chain, we're done.
  MatcherNode *N = MatcherPtr.get();
  if (N == 0) return;
  
  // If we have a scope node, walk down both edges.
  if (ScopeMatcherNode *Push = dyn_cast<ScopeMatcherNode>(N))
    ContractNodes(Push->getCheckPtr());
  
  // If we found a movechild node with a node that comes in a 'foochild' form,
  // transform it.
  if (MoveChildMatcherNode *MC = dyn_cast<MoveChildMatcherNode>(N)) {
    MatcherNode *New = 0;
    if (RecordMatcherNode *RM = dyn_cast<RecordMatcherNode>(MC->getNext()))
      New = new RecordChildMatcherNode(MC->getChildNo(), RM->getWhatFor());
    
    if (CheckTypeMatcherNode *CT= dyn_cast<CheckTypeMatcherNode>(MC->getNext()))
      New = new CheckChildTypeMatcherNode(MC->getChildNo(), CT->getType());
    
    if (New) {
      // Insert the new node.
      New->setNext(MatcherPtr.take());
      MatcherPtr.reset(New);
      // Remove the old one.
      MC->setNext(MC->getNext()->takeNext());
      return ContractNodes(MatcherPtr);
    }
  }
  
  if (MoveChildMatcherNode *MC = dyn_cast<MoveChildMatcherNode>(N))
    if (MoveParentMatcherNode *MP = 
          dyn_cast<MoveParentMatcherNode>(MC->getNext())) {
      MatcherPtr.reset(MP->takeNext());
      return ContractNodes(MatcherPtr);
    }
  
  ContractNodes(N->getNextPtr());
}

MatcherNode *llvm::OptimizeMatcher(MatcherNode *Matcher) {
  OwningPtr<MatcherNode> MatcherPtr(Matcher);
  ContractNodes(MatcherPtr);
  return MatcherPtr.take();
}
