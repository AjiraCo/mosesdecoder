//
//  PhraseDictionaryALSuffixArray.cpp
//  moses
//
//  Created by Hieu Hoang on 06/11/2011.
//  Copyright 2011 __MyCompanyName__. All rights reserved.
//

#include <iostream>
#include "PhraseDictionaryALSuffixArray.h"
#include "moses/InputType.h"
#include "moses/InputFileStream.h"
#include "moses/TypeDef.h"
#include "moses/StaticData.h"
#include "moses/UserMessage.h"
#include "Loader.h"
#include "LoaderFactory.h"

using namespace std;

namespace Moses
{
PhraseDictionaryALSuffixArray::PhraseDictionaryALSuffixArray(const std::string &line)
  : PhraseDictionaryMemory("PhraseDictionaryALSuffixArray", line)
{
  const StaticData &staticData = StaticData::Instance();
  if (staticData.ThreadCount() > 1) {
    throw runtime_error("Suffix array implementation is not threadsafe");
  }
  CHECK(m_args.size() == 0);
}

void PhraseDictionaryALSuffixArray::Load()
{
  SetFeaturesToApply();
}

void PhraseDictionaryALSuffixArray::InitializeForInput(InputType const& source)
{
  // populate with rules for this sentence
  long translationId = source.GetTranslationId();

  string grammarFile = GetFilePath() + "/grammar." + SPrint(translationId) + ".gz";

  std::auto_ptr<RuleTableLoader> loader =
    RuleTableLoaderFactory::Create(grammarFile);
  bool ret = loader->Load(m_input, m_output, grammarFile, m_tableLimit,
                          *this);

  CHECK(ret);
}

void PhraseDictionaryALSuffixArray::CleanUpAfterSentenceProcessing(const InputType &source)
{
  m_collection.Clear();
}

}
