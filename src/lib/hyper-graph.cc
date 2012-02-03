#include <kyldr/hyper-edge.h>
#include <kyldr/hyper-graph.h>
#include <kyldr/hypothesis-queue.h>
#include <kyldr/feature-vector.h>
#include <kyldr/feature-set.h>
#include <kyldr/reorderer-model.h>
#include <kyldr/target-span.h>
#include <kyldr/util.h>
#include <iostream>

using namespace kyldr;
using namespace std;

template <class T>
struct DescendingScore {
  bool operator ()(T *lhs, T *rhs) { return rhs->GetScore() < lhs->GetScore(); }
};

// Return the edge feature vector
const FeatureVectorInt * HyperGraph::GetEdgeFeatures(
                                FeatureSet & feature_gen,
                                const Sentence & sent,
                                const HyperEdge & edge) {
    FeatureVectorInt * ret;
    EdgeFeatureMap::const_iterator it = features_.find(edge);
    if(it == features_.end()) {
        ret = feature_gen.MakeEdgeFeatures(sent, edge);
        features_.insert(MakePair(edge, ret));
    } else {
        ret = it->second;
    }
    return ret;
}

// Score a span
double HyperGraph::Score(const ReordererModel & model,
                         double loss_multiplier,
                         TargetSpan* span) {
    double max_score = span->GetScore();
    if(max_score == -DBL_MAX) {
        std::vector<Hypothesis*> & hyps = span->GetHypotheses();
        BOOST_FOREACH(Hypothesis *hyp, hyps)
            max_score = max(max_score, Score(model, loss_multiplier, hyp));
        sort(hyps.begin(), hyps.end(), DescendingScore<Hypothesis>());
    }
    return max_score;
}

// Score a hypothesis
double HyperGraph::Score(const ReordererModel & model,
                         double loss_multiplier,
                         Hypothesis* hyp) {
    double score = hyp->GetScore();
    if(score == -DBL_MAX) { 
        score = hyp->GetLoss()*loss_multiplier;
        int l = hyp->GetLeft(), c = hyp->GetCenter(), r = hyp->GetRight();
        HyperEdge::Type t = hyp->GetType();
        if(t != HyperEdge::EDGE_ROOT) {
            EdgeFeatureMap::const_iterator fit = 
                                        features_.find(HyperEdge(l,c,r,t));
            if(fit == features_.end())
                THROW_ERROR("No features found in Score for l="
                                        <<l<<", c="<<c<<", r="<<r<<", t="<<(char)t);
            score += model.ScoreFeatureVector(*fit->second);
        }
        if(hyp->GetLeftChild()) 
            score += Score(model, loss_multiplier, hyp->GetLeftChild());
        if(hyp->GetRightChild()) 
            score += Score(model, loss_multiplier, hyp->GetRightChild());
        hyp->SetScore(score);
    }
    return score;
}

double HyperGraph::Rescore(const ReordererModel & model, double loss_multiplier) {
    // Reset everything to -DBL_MAX to indicate it needs to be recalculated
    BOOST_FOREACH(SpanStack * stack, stacks_)
        BOOST_FOREACH(TargetSpan * trg, stack->GetSpans())
            BOOST_FOREACH(Hypothesis * hyp, trg->GetHypotheses())
                hyp->SetScore(-DBL_MAX);
    // Recursively score all edges from the root
    BOOST_FOREACH(TargetSpan * trg, (*stacks_.rbegin())->GetSpans())
        Score(model, loss_multiplier, trg);
    // Sort to make sure that the spans are all in the right order 
    BOOST_FOREACH(SpanStack * stack, stacks_)
        sort(stack->GetSpans().begin(), stack->GetSpans().end(), 
                                            DescendingScore<TargetSpan>()); 
    TargetSpan* best = (*stacks_.rbegin())->GetSpanOfRank(0);
    return best->GetScore();
}

// Get the score for a single edge
double HyperGraph::GetEdgeScore(const ReordererModel & model,
                                FeatureSet & feature_gen,
                                const Sentence & sent,
                                const HyperEdge & edge) {
    const FeatureVectorInt * vec = 
                GetEdgeFeatures(feature_gen, sent, edge);
    return model.ScoreFeatureVector(SafeReference(vec));
}

// Build a hypergraph using beam search and cube pruning
SpanStack * HyperGraph::ProcessOneSpan(const ReordererModel & model,
                                       FeatureSet & features,
                                       const Sentence & sent,
                                       int l, int r,
                                       int beam_size) {
    // Create the temporary data members for this span
    HypothesisQueue q;
    double score;
    // If the length is OK, add a terminal
    if((features.GetMaxTerm() == 0) || (r-l < features.GetMaxTerm())) {
        // Create a hypothesis with the forward terminal
        score = GetEdgeScore(model, features, sent,
                                HyperEdge(l, -1, r, HyperEdge::EDGE_FOR));
        q.push(Hypothesis(score, l, r, l, r, HyperEdge::EDGE_FOR));
        // Create a hypothesis with the backward terminal
        score = GetEdgeScore(model, features, sent, 
                                HyperEdge(l, -1, r, HyperEdge::EDGE_BAC));
        q.push(Hypothesis(score, l, r, r, l, HyperEdge::EDGE_BAC));
    }
    TargetSpan *left_trg, *right_trg, 
               *new_left_trg, *old_left_trg,
               *new_right_trg, *old_right_trg;
    // Add the best hypotheses for each non-terminal to the queue
    for(int c = l+1; c <= r; c++) {
        // Find the best hypotheses on the left and right side
        left_trg = GetTrgSpan(l, c-1, 0);
        right_trg = GetTrgSpan(c, r, 0);
        // Add the straight terminal
        score = left_trg->GetScore() + right_trg->GetScore() + 
                  GetEdgeScore(model, features, sent, 
                                HyperEdge(l, c, r, HyperEdge::EDGE_STR));
        q.push(Hypothesis(score, l, r,
                         left_trg->GetTrgLeft(), right_trg->GetTrgRight(),
                         HyperEdge::EDGE_STR, c, 0, 0, left_trg, right_trg));
        // Add the inverted terminal
        score = left_trg->GetScore() + right_trg->GetScore() + 
                  GetEdgeScore(model, features, sent, 
                                HyperEdge(l, c, r, HyperEdge::EDGE_INV));
        q.push(Hypothesis(score, l, r,
                         right_trg->GetTrgLeft(), left_trg->GetTrgRight(),
                         HyperEdge::EDGE_INV, c, 0, 0, left_trg, right_trg));

    }
    // Get a map to store identical target spans
    map<pair<int,int>, TargetSpan*> spans;
    // Start beam search
    int num_processed = 0;
    while((!beam_size || num_processed < beam_size) && q.size()) {
        // Pop a hypothesis from the stack and get its target span
        Hypothesis hyp = q.top(); q.pop();
        TargetSpan * trg_span;
        pair<int,int> trg_idx = MakePair(hyp.GetTrgLeft(), hyp.GetTrgRight());
        map<pair<int,int>, TargetSpan*>::iterator it = spans.find(trg_idx);
        if(it != spans.end()) {
            trg_span = it->second;
        } else {
            trg_span = new TargetSpan(hyp.GetLeft(), hyp.GetRight(), 
                                      hyp.GetTrgLeft(), hyp.GetTrgRight());
            spans.insert(MakePair(trg_idx, trg_span));
        }
        // Insert the hypothesis
        trg_span->AddHypothesis(hyp);
        num_processed++;
        // If the next hypothesis on the stack is equal to the current
        // hypothesis, remove it, as this just means that we added the same
        // hypothesis
        while(q.size() && q.top() == hyp) q.pop();
        // Skip terminals
        if(hyp.GetCenter() == -1) continue;
        // Increment the left side if there is still a hypothesis left
        new_left_trg = GetTrgSpan(l, hyp.GetCenter()-1, hyp.GetLeftRank()+1);
        if(new_left_trg) {
            old_left_trg = GetTrgSpan(l,hyp.GetCenter()-1,hyp.GetLeftRank());
            Hypothesis new_hyp(hyp);
            new_hyp.SetScore(hyp.GetScore() 
                        - old_left_trg->GetScore() + new_left_trg->GetScore());
            new_hyp.SetLeftRank(hyp.GetLeftRank()+1);
            new_hyp.SetLeftChild(new_left_trg);
            if(new_hyp.GetType() == HyperEdge::EDGE_STR) {
                new_hyp.SetTrgLeft(new_left_trg->GetTrgLeft());
            } else {
                new_hyp.SetTrgRight(new_left_trg->GetTrgRight());
            }
            q.push(new_hyp);
        }
        // Increment the right side if there is still a hypothesis right
        new_right_trg = GetTrgSpan(hyp.GetCenter(),r,hyp.GetRightRank()+1);
        if(new_right_trg) {
            old_right_trg = GetTrgSpan(hyp.GetCenter(),r,hyp.GetRightRank());
            Hypothesis new_hyp(hyp);
            new_hyp.SetScore(hyp.GetScore() 
                    - old_right_trg->GetScore() + new_right_trg->GetScore());
            new_hyp.SetRightRank(hyp.GetRightRank()+1);
            new_hyp.SetRightChild(new_right_trg);
            if(new_hyp.GetType() == HyperEdge::EDGE_STR) {
                new_hyp.SetTrgRight(new_right_trg->GetTrgRight());
            } else {
                new_hyp.SetTrgLeft(new_right_trg->GetTrgLeft());
            }
            q.push(new_hyp);
        }
    }
    SpanStack * ret = new SpanStack;
    typedef pair<pair<int,int>, TargetSpan*> MapPair;
    BOOST_FOREACH(const MapPair & map_pair, spans)
        ret->AddSpan(map_pair.second);
    sort(ret->GetSpans().begin(), ret->GetSpans().end(), 
                                DescendingScore<TargetSpan>());
    return ret;
}

// Build a hypergraph using beam search and cube pruning
void HyperGraph::BuildHyperGraph(const ReordererModel & model,
                                 FeatureSet & features,
                                 const Sentence & sent,
                                 int beam_size) {
    int n = sent[0]->GetNumWords();
    // Iterate through the right side of the span
    for(int r = 0; r < n; r++) {
        // Move the span from l to r, building hypotheses from small to large
        for(int l = r; l >= 0; l--) {
            SetStack(l, r, ProcessOneSpan(model, features, sent, 
                                          l, r, beam_size));
        }
    }
    // Build the root node
    SpanStack * top = GetStack(0,n-1);
    SpanStack * root_stack = new SpanStack;
    for(int i = 0; i < (int)top->size(); i++) {
        TargetSpan * root = new TargetSpan(0, n-1, (*top)[i]->GetTrgLeft(), (*top)[i]->GetTrgRight());
        root->AddHypothesis(Hypothesis((*top)[i]->GetScore(), 0, n-1, 0, n-1,
                                    HyperEdge::EDGE_ROOT, -1, i, -1, (*top)[i]));
        root_stack->AddSpan(root);
    }
    stacks_.push_back(root_stack);
}

// Add up the loss over an entire subtree defined by span
double HyperGraph::AccumulateLoss(const TargetSpan* span) {
    const Hypothesis * hyp = span->GetHypothesis(0);
    double score = hyp->GetLoss();
    if(hyp->GetLeftChild())  score += AccumulateLoss(hyp->GetLeftChild());
    if(hyp->GetRightChild())  score += AccumulateLoss(hyp->GetRightChild());
    return score;
}

FeatureVectorInt HyperGraph::AccumulateFeatures(const TargetSpan* span) {
    std::map<int,double> feat_map;
    AccumulateFeatures(span, feat_map);
    FeatureVectorInt ret;
    BOOST_FOREACH(FeaturePairInt feat_pair, feat_map)
        ret.push_back(feat_pair);
    return ret;
}

void HyperGraph::AccumulateFeatures(const TargetSpan* span, 
                        std::map<int,double> & feat_map) {
    const Hypothesis * hyp = span->GetHypothesis(0);
    int l = hyp->GetLeft(), c = hyp->GetCenter(), r = hyp->GetRight();
    HyperEdge::Type t = hyp->GetType();
    // Find the features
    if(hyp->GetType() != HyperEdge::EDGE_ROOT) {
        EdgeFeatureMap::const_iterator fit = 
                                    features_.find(HyperEdge(l,c,r,t));
        if(fit == features_.end())
            THROW_ERROR("No features found in Accumulate for l="
                                    <<l<<", c="<<c<<", r="<<r<<", t="<<t);
        BOOST_FOREACH(FeaturePairInt feat_pair, *(fit->second))
            feat_map[feat_pair.first] += feat_pair.second;
    }
    if(hyp->GetLeftChild()) AccumulateFeatures(hyp->GetLeftChild(), feat_map);
    if(hyp->GetRightChild())AccumulateFeatures(hyp->GetRightChild(),feat_map);
}
