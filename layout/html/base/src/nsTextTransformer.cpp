/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied.  See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is Mozilla Communicator client code.
 *
 * The Initial Developer of the Original Code is Netscape Communications
 * Corporation.  Portions created by Netscape are Copyright (C) 1998
 * Netscape Communications Corporation.  All Rights Reserved.
 */
#include "nsCOMPtr.h"
#include "nsTextTransformer.h"
#include "nsIContent.h"
#include "nsIFrame.h"
#include "nsIStyleContext.h"
#include "nsITextContent.h"
#include "nsStyleConsts.h"
#include "nsILineBreaker.h"
#include "nsIWordBreaker.h"
#include "nsHTMLIIDs.h"
#include "nsIServiceManager.h"
#include "nsUnicharUtilCIID.h"
#include "nsICaseConversion.h"
#include "prenv.h"

nsAutoTextBuffer::nsAutoTextBuffer()
  : mBuffer(mAutoBuffer),
    mBufferLen(NS_TEXT_TRANSFORMER_AUTO_WORD_BUF_SIZE)
{
}

nsAutoTextBuffer::~nsAutoTextBuffer()
{
  if (mBuffer && (mBuffer != mAutoBuffer)) {
    delete [] mBuffer;
  }
}

nsresult
nsAutoTextBuffer::GrowBy(PRInt32 aAtLeast, PRBool aCopyToHead)
{
  PRInt32 newSize = mBufferLen * 2;
  if (newSize < mBufferLen + aAtLeast) {
    newSize = mBufferLen + aAtLeast + 100;
  }
  return GrowTo(newSize, aCopyToHead);
}

nsresult
nsAutoTextBuffer::GrowTo(PRInt32 aNewSize, PRBool aCopyToHead)
{
  if (aNewSize > mBufferLen) {
    PRUnichar* newBuffer = new PRUnichar[aNewSize];
    if (!newBuffer) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
    nsCRT::memcpy(&newBuffer[aCopyToHead ? 0 : mBufferLen],
                  mBuffer, sizeof(PRUnichar) * mBufferLen);
    if (mBuffer != mAutoBuffer) {
      delete [] mBuffer;
    }
    mBuffer = newBuffer;
    mBufferLen = aNewSize;
  }
  return NS_OK;
}

//----------------------------------------------------------------------

static NS_DEFINE_IID(kUnicharUtilCID, NS_UNICHARUTIL_CID);
static NS_DEFINE_IID(kICaseConversionIID, NS_ICASECONVERSION_IID);

static nsICaseConversion* gCaseConv =  nsnull;

nsresult
nsTextTransformer::Initialize()
{
  nsresult res = NS_OK;
  if (!gCaseConv) {
    res = nsServiceManager::GetService(kUnicharUtilCID, kICaseConversionIID,
                                       (nsISupports**)&gCaseConv);
    NS_ASSERTION( NS_SUCCEEDED(res), "cannot get UnicharUtil");
    NS_ASSERTION( gCaseConv != NULL, "cannot get UnicharUtil");
  }
  return res;
}

void
nsTextTransformer::Shutdown()
{
  if (gCaseConv) {
    nsServiceManager::ReleaseService(kUnicharUtilCID, gCaseConv);
    gCaseConv = nsnull;
  }
}

// XXX I'm sure there are other special characters
#define CH_NBSP 160

#define MAX_UNIBYTE 127

MOZ_DECL_CTOR_COUNTER(nsTextTransformer);

nsTextTransformer::nsTextTransformer(nsILineBreaker* aLineBreaker,
                                     nsIWordBreaker* aWordBreaker)
  : mHasMultibyte(PR_FALSE),
    mFrag(nsnull),
    mOffset(0),
    mTextTransform(NS_STYLE_TEXT_TRANSFORM_NONE),
    mMode(eNormal),
    mLineBreaker(aLineBreaker),
    mWordBreaker(aWordBreaker)
{
  MOZ_COUNT_CTOR(nsTextTransformer);

#ifdef DEBUG
  static PRBool firstTime = PR_TRUE;
  if (firstTime) {
    firstTime = PR_FALSE;
    SelfTest(aLineBreaker, aWordBreaker);
  }
#endif
}

nsTextTransformer::~nsTextTransformer()
{
  MOZ_COUNT_DTOR(nsTextTransformer);
}

nsresult
nsTextTransformer::Init(nsIFrame* aFrame,
                        nsIContent* aContent,
                        PRInt32 aStartingOffset)
{
  // Get the contents text content
  nsresult rv;
  nsCOMPtr<nsITextContent> tc = do_QueryInterface(aContent, &rv);
  if (tc.get()) {
    tc->GetText(&mFrag);

    // Sanitize aStartingOffset
    if (NS_WARN_IF_FALSE(aStartingOffset >= 0, "bad starting offset")) {
      aStartingOffset = 0;
    }
    else if (NS_WARN_IF_FALSE(aStartingOffset <= mFrag->GetLength(),
                              "bad starting offset")) {
      aStartingOffset = mFrag->GetLength();
    }
    mOffset = aStartingOffset;

    // Get the frames text style information
    const nsStyleText* styleText;
    aFrame->GetStyleData(eStyleStruct_Text, (const nsStyleStruct*&) styleText);
    if (NS_STYLE_WHITESPACE_PRE == styleText->mWhiteSpace) {
      mMode = ePreformatted;
    }
    else if (NS_STYLE_WHITESPACE_MOZ_PRE_WRAP == styleText->mWhiteSpace) {
      mMode = ePreWrap;
    }
    mTextTransform = styleText->mTextTransform;
  }
  return rv;
}

//----------------------------------------------------------------------

// wordlen==1, contentlen=newOffset-currentOffset, isWhitespace=t
PRInt32
nsTextTransformer::ScanNormalWhiteSpace_F()
{
  const nsTextFragment* frag = mFrag;
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;

  for (; offset < fragLen; offset++) {
    PRUnichar ch = frag->CharAt(offset);
    if (!XP_IS_SPACE(ch)) {
      break;
    }
  }

  mTransformBuf.mBuffer[0] = ' ';
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanNormalAsciiText_F(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBuffer();
  PRUnichar* endbp = mTransformBuf.GetBufferEnd();

  for (; offset < fragLen; offset++) {
    PRUnichar ch = frag->CharAt(offset);
    if (XP_IS_SPACE(ch)) {
      break;
    }
    if (bp == endbp) {
      PRInt32 oldLength = bp - mTransformBuf.GetBuffer();
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBuffer() + oldLength;
      endbp = mTransformBuf.GetBufferEnd();
    }
    *bp++ = ch;
  }

  *aWordLen = bp - mTransformBuf.GetBuffer();
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanNormalUnicodeText_F(PRBool aForLineBreak,
                                           PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  const PRUnichar* cp0 = frag->Get2b();
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;

  PRUnichar firstChar = frag->CharAt(offset++);
  mTransformBuf.mBuffer[0] = firstChar;
  if (firstChar > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;

  // Only evaluate complex breaking logic if there are more characters
  // beyond the first to look at.
  PRInt32 numChars = 1;
  if (offset < fragLen) {
    const PRUnichar* cp = cp0 + offset;
    PRBool breakBetween = PR_FALSE;
    if (aForLineBreak) {
      mLineBreaker->BreakInBetween(mTransformBuf.GetBuffer(), 1,
                                   cp, (fragLen-offset), &breakBetween);
    }
    else {
      mWordBreaker->BreakInBetween(mTransformBuf.GetBuffer(), 1,
                                   cp, (fragLen-offset), &breakBetween);
    }

    if (!breakBetween) {
      // Find next position
      PRBool tryNextFrag;
      PRUint32 next;
      if (aForLineBreak) {
        mLineBreaker->Next(cp0, fragLen, offset, &next, &tryNextFrag);
      }
      else {
        mWordBreaker->Next(cp0, fragLen, offset, &next, &tryNextFrag);
      }
      numChars = (PRInt32) (next - (PRUint32) offset) + 1;

      // Grow buffer before copying
      nsresult rv = mTransformBuf.GrowTo(numChars);
      if (NS_FAILED(rv)) {
        numChars = mTransformBuf.GetBufferLength();
      }

      // 1. convert nbsp into space
      // 2. check mHasMultibyte flag
      // 3. copy buffer
      PRUnichar* bp = mTransformBuf.GetBuffer() + 1;
      const PRUnichar* end = cp + numChars - 1;
      while (cp < end) {
        PRUnichar ch = *cp++;
        if (CH_NBSP == ch) ch = ' ';
        if (ch > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;
        *bp++ = ch;
      }
    }

  }

  *aWordLen = numChars;
  return offset + numChars - 1;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=t
PRInt32
nsTextTransformer::ScanPreWrapWhiteSpace_F(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBuffer();
  PRUnichar* endbp = mTransformBuf.GetBufferEnd();

  for (; offset < fragLen; offset++) {
    PRUnichar ch = frag->CharAt(offset);
    if (!XP_IS_SPACE(ch) || (ch == '\t') || (ch == '\n')) {
      break;
    }
    if (bp == endbp) {
      PRInt32 oldLength = bp - mTransformBuf.GetBuffer();
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBuffer() + oldLength;
      endbp = mTransformBuf.GetBufferEnd();
    }
    *bp++ = ' ';
  }

  *aWordLen = bp - mTransformBuf.GetBuffer();
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanPreData_F(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBuffer();
  PRUnichar* endbp = mTransformBuf.GetBufferEnd();

  for (; offset < fragLen; offset++) {
    PRUnichar ch = frag->CharAt(offset);
    if ((ch == '\t') || (ch == '\n')) {
      break;
    }
    if (ch > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;
    if (bp == endbp) {
      PRInt32 oldLength = bp - mTransformBuf.GetBuffer();
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBuffer() + oldLength;
      endbp = mTransformBuf.GetBufferEnd();
    }
    *bp++ = ch;
  }

  *aWordLen = bp - mTransformBuf.GetBuffer();
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanPreAsciiData_F(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRUnichar* bp = mTransformBuf.GetBuffer();
  PRUnichar* endbp = mTransformBuf.GetBufferEnd();
  const char* cp = frag->Get1b();
  const char* end = cp + frag->GetLength();
  cp += mOffset;

  while (cp < end) {
    PRUnichar ch = (PRUnichar) *cp++;
    if ((ch == '\t') || (ch == '\n')) {
      cp--;
      break;
    }
    if (bp == endbp) {
      PRInt32 oldLength = bp - mTransformBuf.GetBuffer();
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBuffer() + oldLength;
      endbp = mTransformBuf.GetBufferEnd();
    }
    *bp++ = ch;
  }

  *aWordLen = bp - mTransformBuf.GetBuffer();
  return cp - frag->Get1b();
}

//----------------------------------------

PRUnichar*
nsTextTransformer::GetNextWord(PRBool aInWord,
                               PRInt32* aWordLenResult,
                               PRInt32* aContentLenResult,
                               PRBool* aIsWhiteSpaceResult,
                               PRBool aForLineBreak)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 fragLen = frag->GetLength();
  PRInt32 offset = mOffset;
  PRInt32 wordLen = 0;
  PRBool isWhitespace = PR_FALSE;
  PRUnichar* result = nsnull;

  if (offset < fragLen) {
    PRUnichar firstChar = frag->CharAt(offset);
    switch (mMode) {
      default:
      case eNormal:
        if (XP_IS_SPACE(firstChar)) {
          offset = ScanNormalWhiteSpace_F();
          wordLen = 1;
          isWhitespace = PR_TRUE;
        }
        else if (frag->Is2b()) {
          offset = ScanNormalUnicodeText_F(aForLineBreak, &wordLen);
        }
        else {
          offset = ScanNormalAsciiText_F(&wordLen);
        }
        break;

      case ePreformatted:
        if (('\n' == firstChar) || ('\t' == firstChar)) {
          mTransformBuf.mBuffer[0] = firstChar;
          offset++;
          wordLen = 1;
          isWhitespace = PR_TRUE;
        }
        else if (frag->Is2b()) {
          offset = ScanPreData_F(&wordLen);
        }
        else {
          offset = ScanPreAsciiData_F(&wordLen);
        }
        break;

      case ePreWrap:
        if (XP_IS_SPACE(firstChar)) {
          if (('\n' == firstChar) || ('\t' == firstChar)) {
            mTransformBuf.mBuffer[0] = firstChar;
            offset++;
            wordLen = 1;
          }
          else {
            offset = ScanPreWrapWhiteSpace_F(&wordLen);
          }
          isWhitespace = PR_TRUE;
        }
        else if (frag->Is2b()) {
          offset = ScanNormalUnicodeText_F(aForLineBreak, &wordLen);
        }
        else {
          offset = ScanNormalAsciiText_F(&wordLen);
        }
        break;
    }
    result = mTransformBuf.GetBuffer();

    if (!isWhitespace) {
      switch (mTextTransform) {
        case NS_STYLE_TEXT_TRANSFORM_CAPITALIZE:
          gCaseConv->ToTitle(result, result, wordLen, !aInWord);
          break;
        case NS_STYLE_TEXT_TRANSFORM_LOWERCASE:
          gCaseConv->ToLower(result, result, wordLen);
          break;
        case NS_STYLE_TEXT_TRANSFORM_UPPERCASE:
          gCaseConv->ToUpper(result, result, wordLen);
          break;
      }
    }
  }

  *aWordLenResult = wordLen;
  *aContentLenResult = offset - mOffset;
  *aIsWhiteSpaceResult = isWhitespace;

  mOffset = offset;

  return result;
}

//----------------------------------------------------------------------

// wordlen==1, contentlen=newOffset-currentOffset, isWhitespace=t
PRInt32
nsTextTransformer::ScanNormalWhiteSpace_B()
{
  const nsTextFragment* frag = mFrag;
  PRInt32 offset = mOffset;

  while (--offset >= 0) {
    PRUnichar ch = frag->CharAt(offset);
    if (!XP_IS_SPACE(ch)) {
      break;
    }
  }

  mTransformBuf.mBuffer[mTransformBuf.mBufferLen - 1] = ' ';
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanNormalAsciiText_B(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBufferEnd();
  PRUnichar* startbp = mTransformBuf.GetBuffer();

  while (--offset >= 0) {
    PRUnichar ch = frag->CharAt(offset);
    if (XP_IS_SPACE(ch)) {
      break;
    }
    if (bp == startbp) {
      PRInt32 oldLength = mTransformBuf.mBufferLen;
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBufferEnd() - oldLength;
      startbp = mTransformBuf.GetBuffer();
    }
    *--bp = ch;
  }

  *aWordLen = mTransformBuf.GetBufferEnd() - bp;
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanNormalUnicodeText_B(PRBool aForLineBreak,
                                           PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  const PRUnichar* cp0 = frag->Get2b();
  PRInt32 offset = mOffset - 1;

  PRUnichar firstChar = frag->CharAt(offset);
  mTransformBuf.mBuffer[mTransformBuf.mBufferLen - 1] = firstChar;
  if (firstChar > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;

  PRInt32 numChars = 1;
  if (offset > 0) {
    const PRUnichar* cp = cp0 + offset;
    PRBool breakBetween = PR_FALSE;
    if (aForLineBreak) {
      mLineBreaker->BreakInBetween(cp0, offset + 1,
                                   mTransformBuf.GetBufferEnd()-1, 1,
                                   &breakBetween);
    }
    else {
      mWordBreaker->BreakInBetween(cp0, offset + 1,
                                   mTransformBuf.GetBufferEnd()-1, 1,
                                   &breakBetween);
    }

    if (!breakBetween) {
      // Find next position
      PRBool tryPrevFrag;
      PRUint32 prev;
      if (aForLineBreak) {
        mLineBreaker->Prev(cp0, offset, offset, &prev, &tryPrevFrag);
      }
      else {
        mWordBreaker->Prev(cp0, offset, offset, &prev, &tryPrevFrag);
      }
      numChars = (PRInt32) ((PRUint32) offset - prev) + 1;

      // Grow buffer before copying
      nsresult rv = mTransformBuf.GrowTo(numChars);
      if (NS_FAILED(rv)) {
        numChars = mTransformBuf.GetBufferLength();
      }

      // 1. convert nbsp into space
      // 2. check mHasMultibyte flag
      // 3. copy buffer
      PRUnichar* bp = mTransformBuf.GetBufferEnd() - 1;
      const PRUnichar* end = cp - numChars;
      while (cp > end) {
        PRUnichar ch = *--cp;
        if (CH_NBSP == ch) ch = ' ';
        if (ch > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;
        *--bp = ch;
      }
    }
  }

  *aWordLen = numChars;
  return offset - numChars;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=t
PRInt32
nsTextTransformer::ScanPreWrapWhiteSpace_B(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBufferEnd();
  PRUnichar* startbp = mTransformBuf.GetBuffer();

  while (--offset >= 0) {
    PRUnichar ch = frag->CharAt(offset);
    if (!XP_IS_SPACE(ch) || (ch == '\t') || (ch == '\n')) {
      break;
    }
    if (bp == startbp) {
      PRInt32 oldLength = mTransformBuf.mBufferLen;
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        break;
      }
      bp = mTransformBuf.GetBufferEnd() - oldLength;
      startbp = mTransformBuf.GetBuffer();
    }
    *--bp = ' ';
  }

  *aWordLen = mTransformBuf.GetBufferEnd() - bp;
  return offset;
}

// wordlen==*aWordLen, contentlen=newOffset-currentOffset, isWhitespace=f
PRInt32
nsTextTransformer::ScanPreData_B(PRInt32* aWordLen)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 offset = mOffset;
  PRUnichar* bp = mTransformBuf.GetBufferEnd();
  PRUnichar* startbp = mTransformBuf.GetBuffer();

  while (--offset >= 0) {
    PRUnichar ch = frag->CharAt(offset);
    if ((ch == '\t') || (ch == '\n')) {
      break;
    }
    if (ch > MAX_UNIBYTE) mHasMultibyte = PR_TRUE;
    if (bp == startbp) {
      PRInt32 oldLength = mTransformBuf.mBufferLen;
      nsresult rv = mTransformBuf.GrowBy(1000);
      if (NS_FAILED(rv)) {
        // If we run out of space (unlikely) then just chop the input
        offset++;
        break;
      }
      bp = mTransformBuf.GetBufferEnd() - oldLength;
      startbp = mTransformBuf.GetBuffer();
    }
    *--bp = ch;
  }

  *aWordLen = mTransformBuf.GetBufferEnd() - bp;
  return offset;
}

//----------------------------------------

PRUnichar*
nsTextTransformer::GetPrevWord(PRBool aInWord,
                               PRInt32* aWordLenResult,
                               PRInt32* aContentLenResult,
                               PRBool* aIsWhiteSpaceResult,
                               PRBool aForLineBreak)
{
  const nsTextFragment* frag = mFrag;
  PRInt32 offset = mOffset;
  PRInt32 wordLen = 0;
  PRBool isWhitespace = PR_FALSE;
  PRUnichar* result = nsnull;

  if (--offset >= 0) {
    PRUnichar firstChar = frag->CharAt(offset);
    switch (mMode) {
      default:
      case eNormal:
        if (XP_IS_SPACE(firstChar)) {
          offset = ScanNormalWhiteSpace_B();
          wordLen = 1;
          isWhitespace = PR_TRUE;
        }
        else if (frag->Is2b()) {
          offset = ScanNormalUnicodeText_B(aForLineBreak, &wordLen);
        }
        else {
          offset = ScanNormalAsciiText_B(&wordLen);
        }
        break;

      case ePreformatted:
        if (('\n' == firstChar) || ('\t' == firstChar)) {
          mTransformBuf.mBuffer[mTransformBuf.mBufferLen-1] = firstChar;
          offset--;  // make sure we overshoot
          wordLen = 1;
          isWhitespace = PR_TRUE;
        }
        else {
          offset = ScanPreData_B(&wordLen);
        }
        break;

      case ePreWrap:
        if (XP_IS_SPACE(firstChar)) {
          if (('\n' == firstChar) || ('\t' == firstChar)) {
            mTransformBuf.mBuffer[mTransformBuf.mBufferLen-1] = firstChar;
            offset--;  // make sure we overshoot
            wordLen = 1;
          }
          else {
            offset = ScanPreWrapWhiteSpace_B(&wordLen);
          }
          isWhitespace = PR_TRUE;
        }
        else if (frag->Is2b()) {
          offset = ScanNormalUnicodeText_B(aForLineBreak, &wordLen);
        }
        else {
          offset = ScanNormalAsciiText_B(&wordLen);
        }
        break;
    }

    // Backwards scanning routines *always* overshoot by one for the
    // returned offset value.
    offset = offset + 1;

    result = mTransformBuf.GetBufferEnd() - wordLen;

    if (!isWhitespace) {
      switch (mTextTransform) {
        case NS_STYLE_TEXT_TRANSFORM_CAPITALIZE:
          gCaseConv->ToTitle(result, result, wordLen, !aInWord);
          break;
        case NS_STYLE_TEXT_TRANSFORM_LOWERCASE:
          gCaseConv->ToLower(result, result, wordLen);
          break;
        case NS_STYLE_TEXT_TRANSFORM_UPPERCASE:
          gCaseConv->ToUpper(result, result, wordLen);
          break;
      }
    }
  }

  *aWordLenResult = wordLen;
  *aContentLenResult = mOffset - offset;
  *aIsWhiteSpaceResult = isWhitespace;

  mOffset = offset;

  return result;
}

//----------------------------------------------------------------------

// Self test logic for this class. This will (hopefully) make sure
// that the forward and backward word iterator methods continue to
// function as people change things...

#ifdef DEBUG
struct SelfTestSection {
  int length;
  int* data;
};

#define NUM_MODES 3

struct SelfTestData {
  const PRUnichar* text;
  SelfTestSection modes[NUM_MODES];
};

static PRUint8 preModeValue[NUM_MODES] = {
  NS_STYLE_WHITESPACE_NORMAL,
  NS_STYLE_WHITESPACE_PRE,
  NS_STYLE_WHITESPACE_MOZ_PRE_WRAP
};

static PRUnichar test1text[] = {
  'o', 'n', 'c', 'e', ' ', 'u', 'p', 'o', 'n', '\t',
  'a', ' ', 's', 'h', 'o', 'r', 't', ' ', 't', 'i', 'm', 'e', 0
};
static int test1Results[] = { 4, 1, 4, 1, 1, 1, 5, 1, 4 };
static int test1PreResults[] = { 9, 1, 12 };
static int test1PreWrapResults[] = { 4, 1, 4, 1, 1, 1, 5, 1, 4 };

static PRUnichar test2text[] = {
  0xF6, 'n', 'c', 'e', ' ', 0xFB, 'p', 'o', 'n', '\t',
  0xE3, ' ', 's', 'h', 0xF3, 'r', 't', ' ', 't', 0xEE, 'm', 'e', ' ', 0
};
static int test2Results[] = { 4, 1, 4, 1, 1, 1, 5, 1, 4, 1 };
static int test2PreResults[] = { 9, 1, 13 };
static int test2PreWrapResults[] = { 4, 1, 4, 1, 1, 1, 5, 1, 4, 1 };

static PRUnichar test3text[] = {
  0x0152, 'n', 'c', 'e', ' ', 'x', 'y', '\t', 'z', 'y', ' ', 0
};
static int test3Results[] = { 4, 1, 2, 1, 2, 1, };
static int test3PreResults[] = { 7, 1, 3, };
static int test3PreWrapResults[] = { 4, 1, 2, 1, 2, 1, };

#if 0
static PRUnichar test4text[] = {
  0x30d5, 0x30b8, 0x30c6, 0x30ec, 0x30d3, 0x306e, 0x97f3, 0x697d,
  0x756a, 0x7d44, 0x300c, 'H', 'E', 'Y', '!', ' ', 'H', 'E', 'Y', '!',
  '\t', 'H', 'E', 'Y', '!', 0x300d, 0x306e, 0x30db, 0x30fc, 0x30e0,
  0x30da, 0x30fc, 0x30b8, 0x3002, 0
};
static int test4Results[] = { 1, 1, 1, 1, 1,
                              1, 1, 1, 1, 1,
                              5, 1, 4, 1, 5,
                              1, 2, 1, 2, 2 };
static int test4PreResults[] = { 20, 1, 13 };
static int test4PreWrapResults[] = { 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1,
                                     5, 1, 4, 1, 5,
                                     1, 2, 1, 2, 2 };
#endif

static SelfTestData tests[] = {
  { test1text,
    { { sizeof(test1Results)/sizeof(int), test1Results, },
      { sizeof(test1PreResults)/sizeof(int), test1PreResults, },
      { sizeof(test1PreWrapResults)/sizeof(int), test1PreWrapResults, } }
  },
  { test2text,
    { { sizeof(test2Results)/sizeof(int), test2Results, },
      { sizeof(test2PreResults)/sizeof(int), test2PreResults, },
      { sizeof(test2PreWrapResults)/sizeof(int), test2PreWrapResults, } }
  },
  { test3text,
    { { sizeof(test3Results)/sizeof(int), test3Results, },
      { sizeof(test3PreResults)/sizeof(int), test3PreResults, },
      { sizeof(test3PreWrapResults)/sizeof(int), test3PreWrapResults, } }
  },
#if 0
  { test4text,
    { { sizeof(test4Results)/sizeof(int), test4Results, },
      { sizeof(test4PreResults)/sizeof(int), test4PreResults, },
      { sizeof(test4PreWrapResults)/sizeof(int), test4PreWrapResults, } }
  },
#endif
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

void
nsTextTransformer::SelfTest(nsILineBreaker* aLineBreaker,
                            nsIWordBreaker* aWordBreaker)
{
  PRBool gNoisy = PR_FALSE;
  if (PR_GetEnv("GECKO_TEXT_TRANSFORMER_NOISY_SELF_TEST")) {
    gNoisy = PR_TRUE;
  }

  PRBool error = PR_FALSE;
  PRInt32 testNum = 0;
  SelfTestData* st = tests;
  SelfTestData* last = st + NUM_TESTS;
  for (; st < last; st++) {
    PRUnichar* bp;
    PRInt32 wordLen, contentLen;
    PRBool ws;

    PRBool isAsciiTest = PR_TRUE;
    const PRUnichar* cp = st->text;
    while (*cp) {
      if (*cp > 255) {
        isAsciiTest = PR_FALSE;
        break;
      }
      cp++;
    }

    nsTextFragment frag(st->text);
    nsTextTransformer tx(aLineBreaker, aWordBreaker);

    for (PRInt32 preMode = 0; preMode < NUM_MODES; preMode++) {
      // Do forwards test
      if (gNoisy) {
        nsAutoString uc2(st->text);
        printf("%s forwards test: '", isAsciiTest ? "ascii" : "unicode");
        fputs(uc2, stdout);
        printf("'\n");
      }
      tx.Init2(&frag, 0, preModeValue[preMode], NS_STYLE_TEXT_TRANSFORM_NONE);

      int* expectedResults = st->modes[preMode].data;
      int resultsLen = st->modes[preMode].length;

      while ((bp = tx.GetNextWord(PR_FALSE, &wordLen, &contentLen, &ws))) {
        if (gNoisy) {
          nsAutoString tmp(bp, wordLen);
          printf("  '");
          fputs(tmp, stdout);
          printf("': ws=%s wordLen=%d (%d) contentLen=%d (offset=%d)\n",
                 ws ? "yes" : "no",
                 wordLen, *expectedResults, contentLen, tx.mOffset);
        }
        if (*expectedResults != wordLen) {
          error = PR_TRUE;
          break;
        }
        expectedResults++;
      }
      if (expectedResults != st->modes[preMode].data + resultsLen) {
        error = PR_TRUE;
      }

      // Do backwards test
      if (gNoisy) {
        nsAutoString uc2(st->text);
        printf("%s backwards test: '", isAsciiTest ? "ascii" : "unicode");
        fputs(uc2, stdout);
        printf("'\n");
      }
      tx.Init2(&frag, frag.GetLength(), NS_STYLE_WHITESPACE_NORMAL,
               NS_STYLE_TEXT_TRANSFORM_NONE);
      expectedResults = st->modes[preMode].data + resultsLen;
      while ((bp = tx.GetPrevWord(PR_FALSE, &wordLen, &contentLen, &ws))) {
        --expectedResults;
        if (gNoisy) {
          nsAutoString tmp(bp, wordLen);
          printf("  '");
          fputs(tmp, stdout);
          printf("': ws=%s wordLen=%d contentLen=%d (offset=%d)\n",
                 ws ? "yes" : "no",
                 wordLen, contentLen, tx.mOffset);
        }
        if (*expectedResults != wordLen) {
          error = PR_TRUE;
          break;
        }
      }
      if (expectedResults != st->modes[preMode].data) {
        error = PR_TRUE;
      }

      if (error) {
        fprintf(stderr, "nsTextTransformer: self test %d failed\n", testNum);
      }
      testNum++;
    }
  }
  if (error) {
    NS_ABORT();
  }
}

nsresult
nsTextTransformer::Init2(const nsTextFragment* aFrag,
                         PRInt32 aStartingOffset,
                         PRUint8 aWhiteSpace,
                         PRUint8 aTextTransform)
{
  mFrag = aFrag;

  // Sanitize aStartingOffset
  if (NS_WARN_IF_FALSE(aStartingOffset >= 0, "bad starting offset")) {
    aStartingOffset = 0;
  }
  else if (NS_WARN_IF_FALSE(aStartingOffset <= mFrag->GetLength(),
                            "bad starting offset")) {
    aStartingOffset = mFrag->GetLength();
  }
  mOffset = aStartingOffset;

  // Get the frames text style information
  if (NS_STYLE_WHITESPACE_PRE == aWhiteSpace) {
    mMode = ePreformatted;
  }
  else if (NS_STYLE_WHITESPACE_MOZ_PRE_WRAP == aWhiteSpace) {
    mMode = ePreWrap;
  }
  mTextTransform = aTextTransform;

  return NS_OK;
}
#endif /* DEBUG */
