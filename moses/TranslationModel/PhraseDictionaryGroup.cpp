/***********************************************************************
 Moses - factored phrase-based language decoder
 Copyright (C) 2006 University of Edinburgh

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 ***********************************************************************/

#include "moses/TranslationModel/PhraseDictionaryGroup.h"

#include <boost/foreach.hpp>
#include <boost/unordered_map.hpp>

#include "util/exception.hh"

using namespace std;
using namespace boost;

namespace Moses
{

PhraseDictionaryGroup::PhraseDictionaryGroup(const string &line)
  : PhraseDictionary(line, true),
    m_numModels(0),
    m_restrict(false),
    m_haveDefaultScores(false)
{
  ReadParameters();
}

void PhraseDictionaryGroup::SetParameter(const string& key, const string& value)
{
  if (key == "members") {
    m_memberPDStrs = Tokenize(value, ",");
    m_numModels = m_memberPDStrs.size();
  } else if (key == "restrict") {
    m_restrict = Scan<bool>(value);
  } else if (key =="default-scores") {
    m_haveDefaultScores = true;
    m_defaultScores = Scan<float>(Tokenize(value, ","));
  } else {
    PhraseDictionary::SetParameter(key, value);
  }
}

void PhraseDictionaryGroup::Load()
{
  SetFeaturesToApply();
  m_pdFeature.push_back(const_cast<PhraseDictionaryGroup*>(this));

  // Locate/check component phrase tables
  size_t componentWeights = 0;
  BOOST_FOREACH(const string& pdName, m_memberPDStrs) {
    bool pdFound = false;
    BOOST_FOREACH(PhraseDictionary* pd, PhraseDictionary::GetColl()) {
      if (pd->GetScoreProducerDescription() == pdName) {
        pdFound = true;
        m_memberPDs.push_back(pd);
        componentWeights += pd->GetNumScoreComponents();
      }
    }
    UTIL_THROW_IF2(!pdFound,
                   "Could not find member phrase table " << pdName);
  }
  UTIL_THROW_IF2(componentWeights != m_numScoreComponents,
                 "Total number of member model scores is unequal to specified number of scores");

  // Determine "zero" scores for features
  if (m_haveDefaultScores) {
    UTIL_THROW_IF2(m_defaultScores.size() != m_numScoreComponents,
                   "Number of specified default scores is unequal to number of member model scores");
  } else {
    // Default is all 0 (as opposed to e.g. -99 or similar to approximate log(0)
    // or a smoothed "not in model" score)
    m_defaultScores = vector<float>(m_numScoreComponents, 0);
  }
}

void PhraseDictionaryGroup::InitializeForInput(const ttasksptr& ttask)
{
  // Member models are registered as FFs and should already be initialized
}

void PhraseDictionaryGroup::GetTargetPhraseCollectionBatch(
  const ttasksptr& ttask, const InputPathList& inputPathQueue) const
{
  // Some implementations (mmsapt) do work in PrefixExists
  BOOST_FOREACH(const InputPath* inputPath, inputPathQueue) {
    const Phrase& phrase = inputPath->GetPhrase();
    BOOST_FOREACH(const PhraseDictionary* pd, m_memberPDs) {
      pd->PrefixExists(ttask, phrase);
    }
  }
  // Look up each input in each model
  BOOST_FOREACH(InputPath* inputPath, inputPathQueue) {
    const Phrase &phrase = inputPath->GetPhrase();
    TargetPhraseCollection::shared_ptr  targetPhrases =
      this->GetTargetPhraseCollectionLEGACY(ttask, phrase);
    inputPath->SetTargetPhrases(*this, targetPhrases, NULL);
  }
}

TargetPhraseCollection::shared_ptr  PhraseDictionaryGroup::GetTargetPhraseCollectionLEGACY(
  const Phrase& src) const
{
  UTIL_THROW2("Don't call me without the translation task.");
}

TargetPhraseCollection::shared_ptr
PhraseDictionaryGroup::
GetTargetPhraseCollectionLEGACY(const ttasksptr& ttask, const Phrase& src) const
{
  TargetPhraseCollection::shared_ptr ret
  = CreateTargetPhraseCollection(ttask, src);
  ret->NthElement(m_tableLimit); // sort the phrases for pruning later
  const_cast<PhraseDictionaryGroup*>(this)->CacheForCleanup(ret);
  return ret;
}

TargetPhraseCollection::shared_ptr
PhraseDictionaryGroup::
CreateTargetPhraseCollection(const ttasksptr& ttask, const Phrase& src) const
{
  // Aggregation of phrases and the scores that will be applied to them
  vector<TargetPhrase*> allPhrases;
  // Maps phrase from member model to <phrase copy, scores>
  typedef unordered_map<const TargetPhrase*, pair<TargetPhrase*, vector<float> >, UnorderedComparer<Phrase>, UnorderedComparer<Phrase> > PhraseMap;
  PhraseMap allScores;

  // For each model
  size_t offset = 0;
  for (size_t i = 0; i < m_numModels; ++i) {

    // Collect phrases from this table
    const PhraseDictionary& pd = *m_memberPDs[i];
    TargetPhraseCollection::shared_ptr
    ret_raw = pd.GetTargetPhraseCollectionLEGACY(ttask, src);

    if (ret_raw != NULL) {
      // Process each phrase from table
      BOOST_FOREACH(const TargetPhrase* targetPhrase, *ret_raw) {
        vector<float> raw_scores =
          targetPhrase->GetScoreBreakdown().GetScoresForProducer(&pd);

        // Phrase not in collection -> add if unrestricted or first model
        PhraseMap::iterator iter = allScores.find(targetPhrase);
        if (iter == allScores.end()) {
          if (m_restrict && i > 0) {
            continue;
          }

          // Copy phrase to avoid disrupting base model
          TargetPhrase* phrase = new TargetPhrase(*targetPhrase);
          // Correct future cost estimates and total score
          phrase->GetScoreBreakdown().InvertDenseFeatures(&pd);
          vector<FeatureFunction*> pd_feature;
          pd_feature.push_back(m_memberPDs[i]);
          const vector<FeatureFunction*> pd_feature_const(pd_feature);
          phrase->EvaluateInIsolation(src, pd_feature_const);
          // Zero out scores from original phrase table
          phrase->GetScoreBreakdown().ZeroDenseFeatures(&pd);
          // Add phrase entry
          allPhrases.push_back(phrase);
          allScores[targetPhrase] = make_pair(phrase, vector<float>(m_defaultScores));
        } else {
          // For existing phrases: merge extra scores (such as lr-func scores for mmsapt)
          TargetPhrase* phrase = iter->second.first;
          BOOST_FOREACH(const TargetPhrase::ScoreCache_t::value_type pair, targetPhrase->GetExtraScores()) {
            phrase->SetExtraScores(pair.first, pair.second);
          }
        }
        vector<float>& scores = allScores.find(targetPhrase)->second.second;

        // Copy scores from this model
        for (size_t j = 0; j < pd.GetNumScoreComponents(); ++j) {
          scores[offset + j] = raw_scores[j];
        }
      }
    }
    offset += pd.GetNumScoreComponents();
  }

  // Apply scores to phrases and add them to return collection
  TargetPhraseCollection::shared_ptr ret(new TargetPhraseCollection);
  const vector<FeatureFunction*> pd_feature_const(m_pdFeature);
  BOOST_FOREACH(TargetPhrase* phrase, allPhrases) {
    phrase->GetScoreBreakdown().Assign(this, allScores.find(phrase)->second.second);
    // Correct future cost estimates and total score
    phrase->EvaluateInIsolation(src, pd_feature_const);
    ret->Add(phrase);
  }

  return ret;
}

ChartRuleLookupManager*
PhraseDictionaryGroup::
CreateRuleLookupManager(const ChartParser &,
                        const ChartCellCollectionBase&, size_t)
{
  UTIL_THROW(util::Exception, "Phrase table used in chart decoder");
}

//copied from PhraseDictionaryCompact; free memory allocated to TargetPhraseCollection (and each TargetPhrase) at end of sentence
void PhraseDictionaryGroup::CacheForCleanup(TargetPhraseCollection::shared_ptr  tpc)
{
  PhraseCache &ref = GetPhraseCache();
  ref.push_back(tpc);
}

void
PhraseDictionaryGroup::
CleanUpAfterSentenceProcessing(const InputType &source)
{
  GetPhraseCache().clear();
  // PhraseCache &ref = GetPhraseCache();
  // for (PhraseCache::iterator it = ref.begin(); it != ref.end(); it++) {
  //   delete *it;
  // }

  // PhraseCache temp;
  // temp.swap(ref);

  CleanUpComponentModels(source);
}

void PhraseDictionaryGroup::CleanUpComponentModels(const InputType &source)
{
  for (size_t i = 0; i < m_numModels; ++i) {
    m_memberPDs[i]->CleanUpAfterSentenceProcessing(source);
  }
}

} //namespace
