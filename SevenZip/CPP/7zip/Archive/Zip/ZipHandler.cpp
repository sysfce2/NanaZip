// ZipHandler.cpp

#include "StdAfx.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/StringConvert.h"

#include "../../../Windows/PropVariant.h"
#include "../../../Windows/PropVariantUtils.h"
#include "../../../Windows/TimeUtils.h"

#include "../../IPassword.h"

#include "../../Common/FilterCoder.h"
#include "../../Common/LimitedStreams.h"
#include "../../Common/ProgressUtils.h"
#include "../../Common/StreamObjects.h"
#include "../../Common/StreamUtils.h"

#include "../../Compress/CopyCoder.h"

#ifdef EXTERNAL_CODECS
#ifndef SUPPORT_LZFSE
#define SUPPORT_LZFSE
#endif
#endif

#ifdef SUPPORT_LZFSE
#include "../../Compress/LzfseDecoder.h"
#endif

#include "../../Compress/LzmaDecoder.h"
#include "../../Compress/ImplodeDecoder.h"
#include "../../Compress/PpmdZip.h"
#include "../../Compress/ShrinkDecoder.h"
#include "../../Compress/XzDecoder.h"
#include "../../Compress/ZstdDecoder.h"

#include "../../Crypto/WzAes.h"
#include "../../Crypto/ZipCrypto.h"
#include "../../Crypto/ZipStrong.h"

#include "../Common/ItemNameUtils.h"
#include "../Common/OutStreamWithCRC.h"


#include "ZipHandler.h"

using namespace NWindows;

namespace NArchive {
namespace NZip {

static const char * const kHostOS[] =
{
    "FAT"
  , "AMIGA"
  , "VMS"
  , "Unix"
  , "VM/CMS"
  , "Atari"
  , "HPFS"
  , "Macintosh"
  , "Z-System"
  , "CP/M"
  , "TOPS-20"
  , "NTFS"
  , "SMS/QDOS"
  , "Acorn"
  , "VFAT"
  , "MVS"
  , "BeOS"
  , "Tandem"
  , "OS/400"
  , "OS/X"
};


const char * const kMethodNames1[kNumMethodNames1] =
{
    "Store"
  , "Shrink"
  , "Reduce1"
  , "Reduce2"
  , "Reduce3"
  , "Reduce4"
  , "Implode"
  , NULL // "Tokenize"
  , "Deflate"
  , "Deflate64"
  , "PKImploding"
  , NULL
  , "BZip2"
  , NULL
  , "LZMA"
  , NULL
  , NULL
  , NULL
  , NULL
  , NULL
  , "zstd-pk"
};


const char * const kMethodNames2[kNumMethodNames2] =
{
    "zstd"
  , "MP3"
  , "xz"
  , "Jpeg"
  , "WavPack"
  , "PPMd"
  , "LZFSE" // , "WzAES"
};

#define kMethod_AES "AES"
#define kMethod_ZipCrypto "ZipCrypto"
#define kMethod_StrongCrypto "StrongCrypto"

static const char * const kDeflateLevels[4] =
{
    "Normal"
  , "Maximum"
  , "Fast"
  , "Fastest"
};


static const CUInt32PCharPair g_HeaderCharacts[] =
{
  { 0, "Encrypt" },
  { 3, "Descriptor" },
  // { 5, "Patched" },
  { 6, kMethod_StrongCrypto },
  { 11, "UTF8" },
  { 14, "Alt" }
};

struct CIdToNamePair
{
  unsigned Id;
  const char *Name;
};


static const CIdToNamePair k_StrongCryptoPairs[] =
{
  { NStrongCrypto_AlgId::kDES, "DES" },
  { NStrongCrypto_AlgId::kRC2old, "RC2a" },
  { NStrongCrypto_AlgId::k3DES168, "3DES-168" },
  { NStrongCrypto_AlgId::k3DES112, "3DES-112" },
  { NStrongCrypto_AlgId::kAES128, "pkAES-128" },
  { NStrongCrypto_AlgId::kAES192, "pkAES-192" },
  { NStrongCrypto_AlgId::kAES256, "pkAES-256" },
  { NStrongCrypto_AlgId::kRC2, "RC2" },
  { NStrongCrypto_AlgId::kBlowfish, "Blowfish" },
  { NStrongCrypto_AlgId::kTwofish, "Twofish" },
  { NStrongCrypto_AlgId::kRC4, "RC4" }
};

static const char *FindNameForId(const CIdToNamePair *pairs, unsigned num, unsigned id)
{
  for (unsigned i = 0; i < num; i++)
  {
    const CIdToNamePair &pair = pairs[i];
    if (id == pair.Id)
      return pair.Name;
  }
  return NULL;
}


static const Byte kProps[] =
{
  kpidPath,
  kpidIsDir,
  kpidSize,
  kpidPackSize,
  kpidMTime,
  kpidCTime,
  kpidATime,
  kpidAttrib,
  // kpidPosixAttrib,
  kpidEncrypted,
  kpidComment,
  kpidCRC,
  kpidMethod,
  kpidCharacts,
  kpidHostOS,
  kpidUnpackVer,
  kpidVolumeIndex,
  kpidOffset
  // kpidIsAltStream
};

static const Byte kArcProps[] =
{
  kpidEmbeddedStubSize,
  kpidBit64,
  kpidComment,
  kpidCharacts,
  kpidTotalPhySize,
  kpidIsVolume,
  kpidVolumeIndex,
  kpidNumVolumes
};

CHandler::CHandler()
{
  InitMethodProps();
}

static AString BytesToString(const CByteBuffer &data)
{
  AString s;
  s.SetFrom_CalcLen((const char *)(const Byte *)data, (unsigned)data.Size());
  return s;
}

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

STDMETHODIMP CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value)
{
  COM_TRY_BEGIN
  NWindows::NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidBit64:  if (m_Archive.IsZip64) prop = m_Archive.IsZip64; break;
    case kpidComment:  if (m_Archive.ArcInfo.Comment.Size() != 0) prop = MultiByteToUnicodeString(BytesToString(m_Archive.ArcInfo.Comment), CP_ACP); break;

    case kpidPhySize:  prop = m_Archive.GetPhySize(); break;
    case kpidOffset:  prop = m_Archive.GetOffset(); break;

    case kpidEmbeddedStubSize:
    {
      UInt64 stubSize = m_Archive.GetEmbeddedStubSize();
      if (stubSize != 0)
        prop = stubSize;
      break;
    }

    case kpidTotalPhySize: if (m_Archive.IsMultiVol) prop = m_Archive.Vols.TotalBytesSize; break;
    case kpidVolumeIndex: if (m_Archive.IsMultiVol) prop = (UInt32)m_Archive.Vols.StartVolIndex; break;
    case kpidIsVolume: if (m_Archive.IsMultiVol) prop = true; break;
    case kpidNumVolumes: if (m_Archive.IsMultiVol) prop = (UInt32)m_Archive.Vols.Streams.Size(); break;

    case kpidCharacts:
    {
      AString s;

      if (m_Archive.LocalsWereRead)
      {
        s.Add_OptSpaced("Local");

        if (m_Archive.LocalsCenterMerged)
          s.Add_OptSpaced("Central");
      }

      if (m_Archive.IsZip64)
        s.Add_OptSpaced("Zip64");

      if (m_Archive.IsCdUnsorted)
        s.Add_OptSpaced("Unsorted_CD");

      if (m_Archive.IsApk)
        s.Add_OptSpaced("apk");

      if (m_Archive.ExtraMinorError)
        s.Add_OptSpaced("Minor_Extra_ERROR");

      if (!s.IsEmpty())
        prop = s;
      break;
    }

    case kpidWarningFlags:
    {
      UInt32 v = 0;
      // if (m_Archive.ExtraMinorError) v |= kpv_ErrorFlags_HeadersError;
      if (m_Archive.HeadersWarning) v |= kpv_ErrorFlags_HeadersError;
      if (v != 0)
        prop = v;
      break;
    }

    case kpidWarning:
    {
      AString s;
      if (m_Archive.Overflow32bit)
        s.Add_OptSpaced("32-bit overflow in headers");
      if (m_Archive.Cd_NumEntries_Overflow_16bit)
        s.Add_OptSpaced("16-bit overflow for number of files in headers");
      if (!s.IsEmpty())
        prop = s;
      break;
    }

    case kpidError:
    {
      if (!m_Archive.Vols.MissingName.IsEmpty())
      {
        UString s("Missing volume : ");
        s += m_Archive.Vols.MissingName;
        prop = s;
      }
      break;
    }

    case kpidErrorFlags:
    {
      UInt32 v = 0;
      if (!m_Archive.IsArc) v |= kpv_ErrorFlags_IsNotArc;
      if (m_Archive.HeadersError) v |= kpv_ErrorFlags_HeadersError;
      if (m_Archive.UnexpectedEnd) v |= kpv_ErrorFlags_UnexpectedEnd;
      if (m_Archive.ArcInfo.Base < 0)
      {
        /* We try to support case when we have sfx-zip with embedded stub,
           but the stream has access only to zip part.
           In that case we ignore UnavailableStart error.
           maybe we must show warning in that case. */
        UInt64 stubSize = m_Archive.GetEmbeddedStubSize();
        if (stubSize < (UInt64)-m_Archive.ArcInfo.Base)
          v |= kpv_ErrorFlags_UnavailableStart;
      }
      if (m_Archive.NoCentralDir) v |= kpv_ErrorFlags_UnconfirmedStart;
      prop = v;
      break;
    }

    case kpidReadOnly:
    {
      if (m_Archive.IsOpen())
        if (!m_Archive.CanUpdate())
          prop = true;
      break;
    }

    // case kpidIsAltStream: prop = true; break;
  }
  return prop.Detach(value);
  COM_TRY_END
}

STDMETHODIMP CHandler::GetNumberOfItems(UInt32 *numItems)
{
  *numItems = m_Items.Size();
  return S_OK;
}

STDMETHODIMP CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value)
{
  COM_TRY_BEGIN
  NWindows::NCOM::CPropVariant prop;
  const CItemEx &item = m_Items[index];
  const CExtraBlock &extra = item.GetMainExtra();

  switch (propID)
  {
    case kpidPath:
    {
      UString res;
      item.GetUnicodeString(res, item.Name, false, _forceCodePage, _specifiedCodePage);
      NItemName::ReplaceToOsSlashes_Remove_TailSlash(res,
          item.Is_MadeBy_Unix() // useBackslashReplacement
          );
      /*
      if (item.ParentOfAltStream >= 0)
      {
        const CItemEx &prevItem = m_Items[item.ParentOfAltStream];
        UString prevName;
        prevItem.GetUnicodeString(prevName, prevItem.Name, false, _forceCodePage, _specifiedCodePage);
        NItemName::ReplaceToOsSlashes_Remove_TailSlash(prevName);
        if (res.IsPrefixedBy(prevName))
          if (IsString1PrefixedByString2(res.Ptr(prevName.Len()), k_SpecName_NTFS_STREAM))
          {
            res.Delete(prevName.Len(), (unsigned)strlen(k_SpecName_NTFS_STREAM));
            res.Insert(prevName.Len(), L":");
          }
      }
      */
      prop = res;
      break;
    }

    case kpidIsDir:  prop = item.IsDir(); break;
    case kpidSize:
    {
      if (!item.IsBadDescriptor())
        prop = item.Size;
      break;
    }

    case kpidPackSize:  prop = item.PackSize; break;

    case kpidTimeType:
    {
      FILETIME ft;
      UInt32 unixTime;
      UInt32 type;
      if (extra.GetNtfsTime(NFileHeader::NNtfsExtra::kMTime, ft))
        type = NFileTimeType::kWindows;
      else if (extra.GetUnixTime(true, NFileHeader::NUnixTime::kMTime, unixTime))
        type = NFileTimeType::kUnix;
      else
        type = NFileTimeType::kDOS;
      prop = type;
      break;
    }

    case kpidCTime:
    {
      FILETIME utc;
      bool defined = true;
      if (!extra.GetNtfsTime(NFileHeader::NNtfsExtra::kCTime, utc))
      {
        UInt32 unixTime = 0;
        if (extra.GetUnixTime(true, NFileHeader::NUnixTime::kCTime, unixTime))
          NTime::UnixTimeToFileTime(unixTime, utc);
        else
          defined = false;
      }
      if (defined)
        prop = utc;
      break;
    }

    case kpidATime:
    {
      FILETIME utc;
      bool defined = true;
      if (!extra.GetNtfsTime(NFileHeader::NNtfsExtra::kATime, utc))
      {
        UInt32 unixTime = 0;
        if (extra.GetUnixTime(true, NFileHeader::NUnixTime::kATime, unixTime))
          NTime::UnixTimeToFileTime(unixTime, utc);
        else
          defined = false;
      }
      if (defined)
        prop = utc;

      break;
    }

    case kpidMTime:
    {
      FILETIME utc;
      bool defined = true;
      if (!extra.GetNtfsTime(NFileHeader::NNtfsExtra::kMTime, utc))
      {
        UInt32 unixTime = 0;
        if (extra.GetUnixTime(true, NFileHeader::NUnixTime::kMTime, unixTime))
          NTime::UnixTimeToFileTime(unixTime, utc);
        else
        {
          FILETIME localFileTime;
          if (item.Time == 0)
            defined = false;
          else if (!NTime::DosTimeToFileTime(item.Time, localFileTime) ||
              !LocalFileTimeToFileTime(&localFileTime, &utc))
            utc.dwHighDateTime = utc.dwLowDateTime = 0;
        }
      }
      if (defined)
        prop = utc;
      break;
    }

    case kpidAttrib:  prop = item.GetWinAttrib(); break;

    case kpidPosixAttrib:
    {
      UInt32 attrib;
      if (item.GetPosixAttrib(attrib))
        prop = attrib;
      break;
    }

    case kpidEncrypted:  prop = item.IsEncrypted(); break;

    case kpidComment:
    {
      if (item.Comment.Size() != 0)
      {
        UString res;
        item.GetUnicodeString(res, BytesToString(item.Comment), true, _forceCodePage, _specifiedCodePage);
        prop = res;
      }
      break;
    }

    case kpidCRC:  if (item.IsThereCrc()) prop = item.Crc; break;

    case kpidMethod:
    {
      AString m;
      bool isWzAes = false;
      unsigned id = item.Method;

      if (id == NFileHeader::NCompressionMethod::kWzAES)
      {
        CWzAesExtra aesField;
        if (extra.GetWzAes(aesField))
        {
          m += kMethod_AES;
          m += '-';
          m.Add_UInt32(((unsigned)aesField.Strength + 1) * 64);
          id = aesField.Method;
          isWzAes = true;
        }
      }

      if (item.IsEncrypted())
      if (!isWzAes)
      {
        if (item.IsStrongEncrypted())
        {
          CStrongCryptoExtra f;
          f.AlgId = 0;
          if (extra.GetStrongCrypto(f))
          {
            const char *s = FindNameForId(k_StrongCryptoPairs, ARRAY_SIZE(k_StrongCryptoPairs), f.AlgId);
            if (s)
              m += s;
            else
            {
              m += kMethod_StrongCrypto;
              m += ':';
              m.Add_UInt32(f.AlgId);
            }
            if (f.CertificateIsUsed())
              m += "-Cert";
          }
          else
            m += kMethod_StrongCrypto;
        }
        else
          m += kMethod_ZipCrypto;
      }

      m.Add_Space_if_NotEmpty();

      {
        const char *s = NULL;
        if (id < kNumMethodNames1)
          s = kMethodNames1[id];
        else
        {
          int id2 = (int)id - (int)kMethodNames2Start;
          if (id2 >= 0 && (unsigned)id2 < kNumMethodNames2)
            s = kMethodNames2[id2];
        }
        if (s)
          m += s;
        else
          m.Add_UInt32(id);
      }
      {
        unsigned level = item.GetDeflateLevel();
        if (level != 0)
        {
          if (id == NFileHeader::NCompressionMethod::kLZMA)
          {
            if (level & 1)
              m += ":eos";
            level &= ~(unsigned)1;
          }
          else if (id == NFileHeader::NCompressionMethod::kDeflate)
          {
            m += ':';
            m += kDeflateLevels[level];
            level = 0;
          }

          if (level != 0)
          {
            m += ":v";
            m.Add_UInt32(level);
          }
        }
      }

      prop = m;
      break;
    }

    case kpidCharacts:
    {
      AString s;

      if (item.FromLocal)
      {
        s.Add_OptSpaced("Local");

        item.LocalExtra.PrintInfo(s);

        if (item.FromCentral)
        {
          s.Add_OptSpaced(":");
          s.Add_OptSpaced("Central");
        }
      }

      if (item.FromCentral)
      {
        item.CentralExtra.PrintInfo(s);
      }

      UInt32 flags = item.Flags;
      flags &= ~(unsigned)6; // we don't need compression related bits here.

      if (flags != 0)
      {
        AString s2 = FlagsToString(g_HeaderCharacts, ARRAY_SIZE(g_HeaderCharacts), flags);
        if (!s2.IsEmpty())
        {
          if (!s.IsEmpty())
            s.Add_OptSpaced(":");
          s.Add_OptSpaced(s2);
        }
      }

      if (item.IsBadDescriptor())
        s.Add_OptSpaced("Descriptor_ERROR");

      if (!s.IsEmpty())
        prop = s;
      break;
    }

    case kpidHostOS:
    {
      if (item.FromCentral)
      {
        // 18.06: now we use HostOS only from Central::MadeByVersion
        const Byte hostOS = item.MadeByVersion.HostOS;
        TYPE_TO_PROP(kHostOS, hostOS, prop);
      }
      break;
    }

    case kpidUnpackVer:
      prop = (UInt32)item.ExtractVersion.Version;
      break;

    case kpidVolumeIndex:
      prop = item.Disk;
      break;

    case kpidOffset:
      prop = item.LocalHeaderPos;
      break;

    /*
    case kpidIsAltStream:
      prop = (bool)(item.ParentOfAltStream >= 0); // item.IsAltStream();
      break;

    case kpidName:
      if (item.ParentOfAltStream >= 0)
      {
        // extract name of stream here
      }
      break;
    */
  }

  return prop.Detach(value);
  COM_TRY_END
}



/*
STDMETHODIMP CHandler::GetNumRawProps(UInt32 *numProps)
{
  *numProps = 0;
  return S_OK;
}

STDMETHODIMP CHandler::GetRawPropInfo(UInt32 index, BSTR *name, PROPID *propID)
{
  UNUSED_VAR(index);
  *propID = 0;
  *name = 0;
  return S_OK;
}

STDMETHODIMP CHandler::GetParent(UInt32 index, UInt32 *parent, UInt32 *parentType)
{
  *parentType = NParentType::kDir;
  *parent = (UInt32)(Int32)-1;
  if (index >= m_Items.Size())
    return S_OK;
  const CItemEx &item = m_Items[index];

  if (item.ParentOfAltStream >= 0)
  {
    *parentType = NParentType::kAltStream;
    *parent = item.ParentOfAltStream;
  }
  return S_OK;
}

STDMETHODIMP CHandler::GetRawProp(UInt32 index, PROPID propID, const void **data, UInt32 *dataSize, UInt32 *propType)
{
  UNUSED_VAR(index);
  UNUSED_VAR(propID);
  *data = NULL;
  *dataSize = 0;
  *propType = 0;
  return S_OK;
}


void CHandler::MarkAltStreams(CObjectVector<CItemEx> &items)
{
  int prevIndex = -1;
  UString prevName;
  UString name;

  for (unsigned i = 0; i < items.Size(); i++)
  {
    CItemEx &item = m_Items[i];
    if (item.IsAltStream())
    {
      if (prevIndex == -1)
        continue;
      if (prevName.IsEmpty())
      {
        const CItemEx &prevItem = m_Items[prevIndex];
        prevItem.GetUnicodeString(prevName, prevItem.Name, false, _forceCodePage, _specifiedCodePage);
        NItemName::ReplaceToOsSlashes_Remove_TailSlash(prevName);
      }
      name.Empty();
      item.GetUnicodeString(name, item.Name, false, _forceCodePage, _specifiedCodePage);
      NItemName::ReplaceToOsSlashes_Remove_TailSlash(name);

      if (name.IsPrefixedBy(prevName))
        if (IsString1PrefixedByString2(name.Ptr(prevName.Len()), k_SpecName_NTFS_STREAM))
          item.ParentOfAltStream = prevIndex;
    }
    else
    {
      prevIndex = i;
      prevName.Empty();
    }
  }
}
*/

STDMETHODIMP CHandler::Open(IInStream *inStream,
    const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *callback)
{
  COM_TRY_BEGIN
  try
  {
    Close();
    HRESULT res = m_Archive.Open(inStream, maxCheckStartPosition, callback, m_Items);
    if (res != S_OK)
    {
      m_Items.Clear();
      m_Archive.ClearRefs(); // we don't want to clear error flags
    }
    // MarkAltStreams(m_Items);
    return res;
  }
  catch(...) { Close(); throw; }
  COM_TRY_END
}

STDMETHODIMP CHandler::Close()
{
  m_Items.Clear();
  m_Archive.Close();
  return S_OK;
}

class CZstdDecoder:
  public ICompressCoder,
  public CMyUnknownImp
{
  NCompress::NZSTD::CDecoder *DecoderSpec;
  CMyComPtr<ICompressCoder> Decoder;
public:
  CZstdDecoder();
  STDMETHOD(Code)(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress);

  MY_UNKNOWN_IMP
};

CZstdDecoder::CZstdDecoder()
{
  DecoderSpec = new NCompress::NZSTD::CDecoder;
  Decoder = DecoderSpec;
}

HRESULT CZstdDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  return Decoder->Code(inStream, outStream, NULL, outSize, progress);
}


class CLzmaDecoder:
  public ICompressCoder,
  public ICompressSetFinishMode,
  public ICompressGetInStreamProcessedSize,
  public CMyUnknownImp
{
public:
  NCompress::NLzma::CDecoder *DecoderSpec;
  CMyComPtr<ICompressCoder> Decoder;

  MY_UNKNOWN_IMP2(
      ICompressSetFinishMode,
      ICompressGetInStreamProcessedSize)

  STDMETHOD(Code)(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress);
  STDMETHOD(SetFinishMode)(UInt32 finishMode);
  STDMETHOD(GetInStreamProcessedSize)(UInt64 *value);

  CLzmaDecoder();
};

CLzmaDecoder::CLzmaDecoder()
{
  DecoderSpec = new NCompress::NLzma::CDecoder;
  Decoder = DecoderSpec;
}

static const unsigned kZipLzmaPropsSize = 4 + LZMA_PROPS_SIZE;

HRESULT CLzmaDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  Byte buf[kZipLzmaPropsSize];
  RINOK(ReadStream_FALSE(inStream, buf, kZipLzmaPropsSize));
  if (buf[2] != LZMA_PROPS_SIZE || buf[3] != 0)
    return E_NOTIMPL;
  RINOK(DecoderSpec->SetDecoderProperties2(buf + 4, LZMA_PROPS_SIZE));
  UInt64 inSize2 = 0;
  if (inSize)
  {
    inSize2 = *inSize;
    if (inSize2 < kZipLzmaPropsSize)
      return S_FALSE;
    inSize2 -= kZipLzmaPropsSize;
  }
  return Decoder->Code(inStream, outStream, inSize ? &inSize2 : NULL, outSize, progress);
}

STDMETHODIMP CLzmaDecoder::SetFinishMode(UInt32 finishMode)
{
  DecoderSpec->FinishStream = (finishMode != 0);
  return S_OK;
}

STDMETHODIMP CLzmaDecoder::GetInStreamProcessedSize(UInt64 *value)
{
  *value = DecoderSpec->GetInputProcessedSize() + kZipLzmaPropsSize;
  return S_OK;
}







struct CMethodItem
{
  unsigned ZipMethod;
  CMyComPtr<ICompressCoder> Coder;
};



class CZipDecoder
{
  NCrypto::NZip::CDecoder *_zipCryptoDecoderSpec;
  NCrypto::NZipStrong::CDecoder *_pkAesDecoderSpec;
  NCrypto::NWzAes::CDecoder *_wzAesDecoderSpec;

  CMyComPtr<ICompressFilter> _zipCryptoDecoder;
  CMyComPtr<ICompressFilter> _pkAesDecoder;
  CMyComPtr<ICompressFilter> _wzAesDecoder;

  CFilterCoder *filterStreamSpec;
  CMyComPtr<ISequentialInStream> filterStream;
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  CObjectVector<CMethodItem> methodItems;

  CLzmaDecoder *lzmaDecoderSpec;
public:
  CZipDecoder():
      _zipCryptoDecoderSpec(0),
      _pkAesDecoderSpec(0),
      _wzAesDecoderSpec(0),
      filterStreamSpec(0),
      lzmaDecoderSpec(0)
    {}

  HRESULT Decode(
    DECL_EXTERNAL_CODECS_LOC_VARS
    CInArchive &archive, const CItemEx &item,
    ISequentialOutStream *realOutStream,
    IArchiveExtractCallback *extractCallback,
    ICompressProgressInfo *compressProgress,
    #ifndef _7ZIP_ST
    UInt32 numThreads, UInt64 memUsage,
    #endif
    Int32 &res);
};


static HRESULT SkipStreamData(ISequentialInStream *stream,
    ICompressProgressInfo *progress, UInt64 packSize, UInt64 unpackSize,
    bool &thereAreData)
{
  thereAreData = false;
  const size_t kBufSize = 1 << 12;
  Byte buf[kBufSize];
  UInt64 prev = packSize;
  for (;;)
  {
    size_t size = kBufSize;
    RINOK(ReadStream(stream, buf, &size));
    if (size == 0)
      return S_OK;
    thereAreData = true;
    packSize += size;
    if ((packSize - prev) >= (1 << 22))
    {
      prev = packSize;
      RINOK(progress->SetRatioInfo(&packSize, &unpackSize));
    }
  }
}



class COutStreamWithPadPKCS7:
  public ISequentialOutStream,
  public CMyUnknownImp
{
  CMyComPtr<ISequentialOutStream> _stream;
  UInt64 _size;
  UInt64 _padPos;
  UInt32 _padSize;
  bool _padFailure;
public:
  MY_UNKNOWN_IMP
  STDMETHOD(Write)(const void *data, UInt32 size, UInt32 *processedSize);
  void SetStream(ISequentialOutStream *stream) { _stream = stream; }
  void ReleaseStream() { _stream.Release(); }

  // padSize == 0 means (no_pad Mode)
  void Init(UInt64 padPos, UInt32 padSize)
  {
    _padPos = padPos;
    _padSize = padSize;
    _size = 0;
    _padFailure = false;
  }
  UInt64 GetSize() const { return _size; }
  bool WasPadFailure() const { return _padFailure; }
};


STDMETHODIMP COutStreamWithPadPKCS7::Write(const void *data, UInt32 size, UInt32 *processedSize)
{
  UInt32 written = 0;
  HRESULT result = S_OK;
  if (_size < _padPos)
  {
    const UInt64 rem = _padPos - _size;
    UInt32 num = size;
    if (num > rem)
      num = (UInt32)rem;
    result = _stream->Write(data, num, &written);
    _size += written;
    if (processedSize)
      *processedSize = written;
    if (_size != _padPos || result != S_OK)
      return result;
    size -= written;
    data = ((const Byte *)data) + written;
  }
  _size += size;
  written += size;
  if (processedSize)
    *processedSize = written;
  if (_padSize != 0)
  for (; size != 0; size--)
  {
    if (*(const Byte *)data != _padSize)
      _padFailure = true;
    data = ((const Byte *)data) + 1;
  }
  return result;
}



HRESULT CZipDecoder::Decode(
    DECL_EXTERNAL_CODECS_LOC_VARS
    CInArchive &archive, const CItemEx &item,
    ISequentialOutStream *realOutStream,
    IArchiveExtractCallback *extractCallback,
    ICompressProgressInfo *compressProgress,
    #ifndef _7ZIP_ST
    UInt32 numThreads, UInt64 memUsage,
    #endif
    Int32 &res)
{
  res = NExtract::NOperationResult::kHeadersError;

  CFilterCoder::C_InStream_Releaser inStreamReleaser;
  CFilterCoder::C_Filter_Releaser filterReleaser;

  bool needCRC = true;
  bool wzAesMode = false;
  bool pkAesMode = false;

  bool badDescriptor = item.IsBadDescriptor();
  if (badDescriptor)
    needCRC = false;


  unsigned id = item.Method;

  CWzAesExtra aesField;
  // LZFSE and WinZip's AES use same id - kWzAES.

  if (id == NFileHeader::NCompressionMethod::kWzAES)
  {
    if (item.GetMainExtra().GetWzAes(aesField))
    {
      if (!item.IsEncrypted())
      {
        res = NExtract::NOperationResult::kUnsupportedMethod;
        return S_OK;
      }
      wzAesMode = true;
      needCRC = aesField.NeedCrc();
    }
  }

  if (!wzAesMode)
  if (item.IsEncrypted())
  {
    if (item.IsStrongEncrypted())
    {
      CStrongCryptoExtra f;
      if (!item.CentralExtra.GetStrongCrypto(f))
      {
        res = NExtract::NOperationResult::kUnsupportedMethod;
        return S_OK;
      }
      pkAesMode = true;
    }
  }

  COutStreamWithCRC *outStreamSpec = new COutStreamWithCRC;
  CMyComPtr<ISequentialOutStream> outStream = outStreamSpec;
  outStreamSpec->SetStream(realOutStream);
  outStreamSpec->Init(needCRC);

  CMyComPtr<ISequentialInStream> packStream;

  CLimitedSequentialInStream *limitedStreamSpec = new CLimitedSequentialInStream;
  CMyComPtr<ISequentialInStream> inStream(limitedStreamSpec);

  {
    UInt64 packSize = item.PackSize;
    if (wzAesMode)
    {
      if (packSize < NCrypto::NWzAes::kMacSize)
        return S_OK;
      packSize -= NCrypto::NWzAes::kMacSize;
    }
    RINOK(archive.GetItemStream(item, true, packStream));
    if (!packStream)
    {
      res = NExtract::NOperationResult::kUnavailable;
      return S_OK;
    }
    limitedStreamSpec->SetStream(packStream);
    limitedStreamSpec->Init(packSize);
  }


  res = NExtract::NOperationResult::kDataError;

  CMyComPtr<ICompressFilter> cryptoFilter;

  if (item.IsEncrypted())
  {
    if (wzAesMode)
    {
      id = aesField.Method;
      if (!_wzAesDecoder)
      {
        _wzAesDecoderSpec = new NCrypto::NWzAes::CDecoder;
        _wzAesDecoder = _wzAesDecoderSpec;
      }
      cryptoFilter = _wzAesDecoder;
      if (!_wzAesDecoderSpec->SetKeyMode(aesField.Strength))
      {
        res = NExtract::NOperationResult::kUnsupportedMethod;
        return S_OK;
      }
    }
    else if (pkAesMode)
    {
      if (!_pkAesDecoder)
      {
        _pkAesDecoderSpec = new NCrypto::NZipStrong::CDecoder;
        _pkAesDecoder = _pkAesDecoderSpec;
      }
      cryptoFilter = _pkAesDecoder;
    }
    else
    {
      if (!_zipCryptoDecoder)
      {
        _zipCryptoDecoderSpec = new NCrypto::NZip::CDecoder;
        _zipCryptoDecoder = _zipCryptoDecoderSpec;
      }
      cryptoFilter = _zipCryptoDecoder;
    }

    CMyComPtr<ICryptoSetPassword> cryptoSetPassword;
    RINOK(cryptoFilter.QueryInterface(IID_ICryptoSetPassword, &cryptoSetPassword));
    if (!cryptoSetPassword)
      return E_FAIL;

    if (!getTextPassword)
      extractCallback->QueryInterface(IID_ICryptoGetTextPassword, (void **)&getTextPassword);

    if (getTextPassword)
    {
      CMyComBSTR_Wipe password;
      RINOK(getTextPassword->CryptoGetTextPassword(&password));
      AString_Wipe charPassword;
      if (password)
      {
        UnicodeStringToMultiByte2(charPassword, (LPCOLESTR)password, CP_ACP);
        /*
        if (wzAesMode || pkAesMode)
        {
        }
        else
        {
          // PASSWORD encoding for ZipCrypto:
          // pkzip25 / WinZip / Windows probably use ANSI
          // 7-Zip <  4.43 creates ZIP archives with OEM encoding in password
          // 7-Zip >= 4.43 creates ZIP archives only with ASCII characters in password
          // 7-Zip <  17.00 uses CP_OEMCP for password decoding
          // 7-Zip >= 17.00 uses CP_ACP   for password decoding
        }
        */
      }
      HRESULT result = cryptoSetPassword->CryptoSetPassword(
        (const Byte *)(const char *)charPassword, charPassword.Len());
      if (result != S_OK)
      {
        res = NExtract::NOperationResult::kWrongPassword;
        return S_OK;
      }
    }
    else
    {
      res = NExtract::NOperationResult::kWrongPassword;
      return S_OK;
      // RINOK(cryptoSetPassword->CryptoSetPassword(NULL, 0));
    }
  }

  unsigned m;
  for (m = 0; m < methodItems.Size(); m++)
    if (methodItems[m].ZipMethod == id)
      break;

  if (m == methodItems.Size())
  {
    CMethodItem mi;
    mi.ZipMethod = id;
    if (id == NFileHeader::NCompressionMethod::kStore)
      mi.Coder = new NCompress::CCopyCoder;
    else if (id == NFileHeader::NCompressionMethod::kShrink)
      mi.Coder = new NCompress::NShrink::CDecoder;
    else if (id == NFileHeader::NCompressionMethod::kImplode)
      mi.Coder = new NCompress::NImplode::NDecoder::CCoder;
    else if (id == NFileHeader::NCompressionMethod::kLZMA)
    {
      lzmaDecoderSpec = new CLzmaDecoder;
      mi.Coder = lzmaDecoderSpec;
    }
    else if (id ==NFileHeader::NCompressionMethod::kZstd)
      mi.Coder = new CZstdDecoder();
    else if (id ==NFileHeader::NCompressionMethod::kZstdPk)
      mi.Coder = new CZstdDecoder();
    else if (id == NFileHeader::NCompressionMethod::kXz)
      mi.Coder = new NCompress::NXz::CComDecoder;
    else if (id == NFileHeader::NCompressionMethod::kPPMd)
      mi.Coder = new NCompress::NPpmdZip::CDecoder(true);
    #ifdef SUPPORT_LZFSE
    else if (id == NFileHeader::NCompressionMethod::kWzAES)
      mi.Coder = new NCompress::NLzfse::CDecoder;
    #endif
    else
    {
      CMethodId szMethodID;
      if (id == NFileHeader::NCompressionMethod::kBZip2)
        szMethodID = kMethodId_BZip2;
      else
      {
        if (id > 0xFF)
        {
          res = NExtract::NOperationResult::kUnsupportedMethod;
          return S_OK;
        }
        szMethodID = kMethodId_ZipBase + (Byte)id;
      }

      RINOK(CreateCoder_Id(EXTERNAL_CODECS_LOC_VARS szMethodID, false, mi.Coder));

      if (!mi.Coder)
      {
        res = NExtract::NOperationResult::kUnsupportedMethod;
        return S_OK;
      }
    }
    m = methodItems.Add(mi);
  }

  const CMethodItem &mi = methodItems[m];
  ICompressCoder *coder = mi.Coder;


  #ifndef _7ZIP_ST
  {
    CMyComPtr<ICompressSetCoderMt> setCoderMt;
    coder->QueryInterface(IID_ICompressSetCoderMt, (void **)&setCoderMt);
    if (setCoderMt)
    {
      RINOK(setCoderMt->SetNumberOfThreads(numThreads));
    }
  }
  // if (memUsage != 0)
  {
    CMyComPtr<ICompressSetMemLimit> setMemLimit;
    coder->QueryInterface(IID_ICompressSetMemLimit, (void **)&setMemLimit);
    if (setMemLimit)
    {
      RINOK(setMemLimit->SetMemLimit(memUsage));
    }
  }
  #endif

  {
    CMyComPtr<ICompressSetDecoderProperties2> setDecoderProperties;
    coder->QueryInterface(IID_ICompressSetDecoderProperties2, (void **)&setDecoderProperties);
    if (setDecoderProperties)
    {
      Byte properties = (Byte)item.Flags;
      RINOK(setDecoderProperties->SetDecoderProperties2(&properties, 1));
    }
  }


  bool isFullStreamExpected = (!item.HasDescriptor() || item.PackSize != 0);
  bool needReminderCheck = false;

  bool dataAfterEnd = false;
  bool truncatedError = false;
  bool lzmaEosError = false;
  bool headersError  = false;
  bool padError = false;
  bool readFromFilter = false;

  const bool useUnpackLimit = (id == NFileHeader::NCompressionMethod::kStore
      || !item.HasDescriptor()
      || item.Size >= ((UInt64)1 << 32)
      || item.LocalExtra.IsZip64
      || item.CentralExtra.IsZip64
      );

  {
    HRESULT result = S_OK;
    if (item.IsEncrypted())
    {
      if (!filterStream)
      {
        filterStreamSpec = new CFilterCoder(false);
        filterStream = filterStreamSpec;
      }

      filterReleaser.FilterCoder = filterStreamSpec;
      filterStreamSpec->Filter = cryptoFilter;

      if (wzAesMode)
      {
        result = _wzAesDecoderSpec->ReadHeader(inStream);
        if (result == S_OK)
        {
          if (!_wzAesDecoderSpec->Init_and_CheckPassword())
          {
            res = NExtract::NOperationResult::kWrongPassword;
            return S_OK;
          }
        }
      }
      else if (pkAesMode)
      {
        isFullStreamExpected = false;
        result =_pkAesDecoderSpec->ReadHeader(inStream, item.Crc, item.Size);
        if (result == S_OK)
        {
          bool passwOK;
          result = _pkAesDecoderSpec->Init_and_CheckPassword(passwOK);
          if (result == S_OK && !passwOK)
          {
            res = NExtract::NOperationResult::kWrongPassword;
            return S_OK;
          }
        }
      }
      else
      {
        result = _zipCryptoDecoderSpec->ReadHeader(inStream);
        if (result == S_OK)
        {
          _zipCryptoDecoderSpec->Init_BeforeDecode();

          /* Info-ZIP modification to ZipCrypto format:
               if bit 3 of the general purpose bit flag is set,
               it uses high byte of 16-bit File Time.
             Info-ZIP code probably writes 2 bytes of File Time.
             We check only 1 byte. */

          // UInt32 v1 = GetUi16(_zipCryptoDecoderSpec->_header + NCrypto::NZip::kHeaderSize - 2);
          // UInt32 v2 = (item.HasDescriptor() ? (item.Time & 0xFFFF) : (item.Crc >> 16));

          Byte v1 = _zipCryptoDecoderSpec->_header[NCrypto::NZip::kHeaderSize - 1];
          Byte v2 = (Byte)(item.HasDescriptor() ? (item.Time >> 8) : (item.Crc >> 24));

          if (v1 != v2)
          {
            res = NExtract::NOperationResult::kWrongPassword;
            return S_OK;
          }
        }
      }
    }

    if (result == S_OK)
    {
      CMyComPtr<ICompressSetFinishMode> setFinishMode;
      coder->QueryInterface(IID_ICompressSetFinishMode, (void **)&setFinishMode);
      if (setFinishMode)
      {
        RINOK(setFinishMode->SetFinishMode(BoolToUInt(true)));
      }

      const UInt64 coderPackSize = limitedStreamSpec->GetRem();

      if (id == NFileHeader::NCompressionMethod::kStore && item.IsEncrypted())
      {
        readFromFilter = false;

        COutStreamWithPadPKCS7 *padStreamSpec = NULL;
        CMyComPtr<ISequentialOutStream> padStream;
        UInt32 padSize = 0;

        if (pkAesMode)
        {
          padStreamSpec = new COutStreamWithPadPKCS7;
          padStream = padStreamSpec;
          padSize = _pkAesDecoderSpec->GetPadSize((UInt32)item.Size);
          padStreamSpec->SetStream(outStream);
          padStreamSpec->Init(item.Size, padSize);
        }

        // Here we decode minimal required size, including padding
        const UInt64 expectedSize = item.Size + padSize;
        UInt64 size = coderPackSize;
        if (item.Size > coderPackSize)
          headersError = true;
        else if (expectedSize != coderPackSize)
        {
          headersError = true;
          if (coderPackSize > expectedSize)
            size = expectedSize;
        }

        result = filterStreamSpec->Code(inStream, padStream ?
            (ISequentialOutStream *)padStream :
            (ISequentialOutStream *)outStream,
            NULL, &size, compressProgress);

        if (outStreamSpec->GetSize() != item.Size)
          truncatedError = true;

        if (pkAesMode)
        {
          if (padStreamSpec->GetSize() != size)
            truncatedError = true;
          if (padStreamSpec->WasPadFailure())
            padError = true;
        }
      }
      else
      {
        if (item.IsEncrypted())
        {
          readFromFilter = true;
          inStreamReleaser.FilterCoder = filterStreamSpec;
          RINOK(filterStreamSpec->SetInStream(inStream));

          /* IFilter::Init() does nothing in all zip crypto filters.
          So we can call any Initialize function in CFilterCoder. */

          RINOK(filterStreamSpec->Init_NoSubFilterInit());
          // RINOK(filterStreamSpec->SetOutStreamSize(NULL));
        }

        try {
        result = coder->Code(readFromFilter ?
              (ISequentialInStream *)filterStream :
              (ISequentialInStream *)inStream,
            outStream,
            isFullStreamExpected ? &coderPackSize : NULL,
            // NULL,
            useUnpackLimit ? &item.Size : NULL,
            compressProgress);
        } catch (...) { return E_FAIL; }

        if (result == S_OK)
        {
        CMyComPtr<ICompressGetInStreamProcessedSize> getInStreamProcessedSize;
        coder->QueryInterface(IID_ICompressGetInStreamProcessedSize, (void **)&getInStreamProcessedSize);
        if (getInStreamProcessedSize && setFinishMode)
        {
          UInt64 processed;
          RINOK(getInStreamProcessedSize->GetInStreamProcessedSize(&processed));
          if (processed != (UInt64)(Int64)-1)
          {
            if (pkAesMode)
            {
              const UInt32 padSize = _pkAesDecoderSpec->GetPadSize((UInt32)processed);
              if (processed + padSize > coderPackSize)
                truncatedError = true;
              else
              {
                if (processed + padSize < coderPackSize)
                  dataAfterEnd = true;
                else
                {
                  // here we can PKCS7 padding data from reminder (it can be inside stream buffer in coder).
                  CMyComPtr<ICompressReadUnusedFromInBuf> readInStream;
                  coder->QueryInterface(IID_ICompressReadUnusedFromInBuf, (void **)&readInStream);
                  if (readInStream)
                  {
                    // change pad size, it we support another block size in ZipStron
                    // here we request more to detect error with data after end.
                    const UInt32 kBufSize = NCrypto::NZipStrong::kAesPadAllign + 16;
                    Byte buf[kBufSize];
                    UInt32 processedSize;
                    RINOK(readInStream->ReadUnusedFromInBuf(buf, kBufSize, &processedSize));
                    if (processedSize > padSize)
                      dataAfterEnd = true;
                    else
                    {
                      if (ReadStream_FALSE(filterStream, buf + processedSize, padSize - processedSize) != S_OK)
                        padError = true;
                      else
                      for (unsigned i = 0; i < padSize; i++)
                        if (buf[i] != padSize)
                          padError = true;
                    }
                  }
                }
              }
            }
            else
            {
              if (processed < coderPackSize)
              {
                if (isFullStreamExpected)
                  dataAfterEnd = true;
              }
              else if (processed > coderPackSize)
              {
                // that case is additional check, that can show the bugs in code (coder)
                truncatedError = true;
              }
              needReminderCheck = isFullStreamExpected;
            }
          }
        }
        }
      }

      if (result == S_OK && id == NFileHeader::NCompressionMethod::kLZMA)
        if (!lzmaDecoderSpec->DecoderSpec->CheckFinishStatus(item.IsLzmaEOS()))
          lzmaEosError = true;
    }

    if (result == S_FALSE)
      return S_OK;

    if (result == E_NOTIMPL)
    {
      res = NExtract::NOperationResult::kUnsupportedMethod;
      return S_OK;
    }

    RINOK(result);
  }

  bool crcOK = true;
  bool authOk = true;
  if (needCRC)
    crcOK = (outStreamSpec->GetCRC() == item.Crc);

  if (useUnpackLimit)
    if (outStreamSpec->GetSize() != item.Size)
      truncatedError = true;

  if (wzAesMode)
  {
    const UInt64 unpackSize = outStreamSpec->GetSize();
    const UInt64 packSize = limitedStreamSpec->GetSize();
    bool thereAreData = false;
    // read to the end from filter or from packed stream
    if (SkipStreamData(readFromFilter ?
          (ISequentialInStream *)filterStream :
          (ISequentialInStream *)inStream,
        compressProgress, packSize, unpackSize, thereAreData) != S_OK)
      authOk = false;
    if (needReminderCheck && thereAreData)
      dataAfterEnd = true;

    if (limitedStreamSpec->GetRem() != 0)
      truncatedError = true;
    else
    {
      limitedStreamSpec->Init(NCrypto::NWzAes::kMacSize);
      if (_wzAesDecoderSpec->CheckMac(inStream, authOk) != S_OK)
        authOk = false;
    }
  }

  res = NExtract::NOperationResult::kCRCError;

  if (crcOK && authOk)
  {
    res = NExtract::NOperationResult::kOK;

    if (dataAfterEnd)
      res = NExtract::NOperationResult::kDataAfterEnd;
    else if (padError)
      res = NExtract::NOperationResult::kCRCError;
    else if (truncatedError)
      res = NExtract::NOperationResult::kUnexpectedEnd;
    else if (headersError)
      res = NExtract::NOperationResult::kHeadersError;
    else if (lzmaEosError)
      res = NExtract::NOperationResult::kHeadersError;
    else if (badDescriptor)
      res = NExtract::NOperationResult::kUnexpectedEnd;

    // CheckDescriptor() supports only data descriptor with signature and
    // it doesn't support "old" pkzip's data descriptor without signature.
    // So we disable that check.
    /*
    if (item.HasDescriptor() && archive.CheckDescriptor(item) != S_OK)
      res = NExtract::NOperationResult::kHeadersError;
    */
  }

  return S_OK;
}


STDMETHODIMP CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback)
{
  COM_TRY_BEGIN
  CZipDecoder myDecoder;
  UInt64 totalUnPacked = 0, totalPacked = 0;
  bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = m_Items.Size();
  if (numItems == 0)
    return S_OK;
  UInt32 i;
  for (i = 0; i < numItems; i++)
  {
    const CItemEx &item = m_Items[allFilesMode ? i : indices[i]];
    totalUnPacked += item.Size;
    totalPacked += item.PackSize;
  }
  RINOK(extractCallback->SetTotal(totalUnPacked));

  UInt64 currentTotalUnPacked = 0, currentTotalPacked = 0;
  UInt64 currentItemUnPacked, currentItemPacked;

  CLocalProgress *lps = new CLocalProgress;
  CMyComPtr<ICompressProgressInfo> progress = lps;
  lps->Init(extractCallback, false);

  for (i = 0; i < numItems; i++,
      currentTotalUnPacked += currentItemUnPacked,
      currentTotalPacked += currentItemPacked)
  {
    currentItemUnPacked = 0;
    currentItemPacked = 0;

    lps->InSize = currentTotalPacked;
    lps->OutSize = currentTotalUnPacked;
    RINOK(lps->SetCur());

    CMyComPtr<ISequentialOutStream> realOutStream;
    Int32 askMode = testMode ?
        NExtract::NAskMode::kTest :
        NExtract::NAskMode::kExtract;
    UInt32 index = allFilesMode ? i : indices[i];

    CItemEx item = m_Items[index];
    bool isLocalOffsetOK = m_Archive.IsLocalOffsetOK(item);
    bool skip = !isLocalOffsetOK && !item.IsDir();
    if (skip)
      askMode = NExtract::NAskMode::kSkip;

    currentItemUnPacked = item.Size;
    currentItemPacked = item.PackSize;

    RINOK(extractCallback->GetStream(index, &realOutStream, askMode));

    if (!isLocalOffsetOK)
    {
      RINOK(extractCallback->PrepareOperation(askMode));
      realOutStream.Release();
      RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kUnavailable));
      continue;
    }

    bool headersError = false;

    if (!item.FromLocal)
    {
      bool isAvail = true;
      HRESULT res = m_Archive.ReadLocalItemAfterCdItem(item, isAvail, headersError);
      if (res == S_FALSE)
      {
        if (item.IsDir() || realOutStream || testMode)
        {
          RINOK(extractCallback->PrepareOperation(askMode));
          realOutStream.Release();
          RINOK(extractCallback->SetOperationResult(
              isAvail ?
                NExtract::NOperationResult::kHeadersError :
                NExtract::NOperationResult::kUnavailable));
        }
        continue;
      }
      RINOK(res);
    }

    if (item.IsDir())
    {
      // if (!testMode)
      {
        RINOK(extractCallback->PrepareOperation(askMode));
        realOutStream.Release();
        RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK));
      }
      continue;
    }

    if (!testMode && !realOutStream)
      continue;

    RINOK(extractCallback->PrepareOperation(askMode));

    Int32 res;
    HRESULT hres = myDecoder.Decode(
        EXTERNAL_CODECS_VARS
        m_Archive, item, realOutStream, extractCallback,
        progress,
        #ifndef _7ZIP_ST
        _props._numThreads, _props._memUsage_Decompress,
        #endif
        res);

    RINOK(hres);
    realOutStream.Release();

    if (res == NExtract::NOperationResult::kOK && headersError)
      res = NExtract::NOperationResult::kHeadersError;

    RINOK(extractCallback->SetOperationResult(res))
  }

  lps->InSize = currentTotalPacked;
  lps->OutSize = currentTotalUnPacked;
  return lps->SetCur();
  COM_TRY_END
}

IMPL_ISetCompressCodecsInfo

}}
