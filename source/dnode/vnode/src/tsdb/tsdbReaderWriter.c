/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cos.h"
#include "tsdb.h"

static int32_t tsdbOpenFileImpl(STsdbFD *pFD) {
  int32_t     code = 0;
  const char *path = pFD->path;
  int32_t     szPage = pFD->szPage;
  int32_t     flag = pFD->flag;

  pFD->pFD = taosOpenFile(path, flag);
  if (pFD->pFD == NULL) {
    int         errsv = errno;
    const char *object_name = taosDirEntryBaseName((char *)path);
    long        s3_size = 0;
    if (tsS3Enabled) {
      long size = s3Size(object_name);
      if (size < 0) {
        code = terrno = TSDB_CODE_FAILED_TO_CONNECT_S3;
        goto _exit;
      }

      s3_size = size;
    }

    if (tsS3Enabled && !strncmp(path + strlen(path) - 5, ".data", 5) && s3_size > 0) {
#ifndef S3_BLOCK_CACHE
      s3EvictCache(path, s3_size);
      s3Get(object_name, path);

      pFD->pFD = taosOpenFile(path, flag);

      if (pFD->pFD == NULL) {
        code = TAOS_SYSTEM_ERROR(ENOENT);
        // taosMemoryFree(pFD);
        goto _exit;
      }
#else
      pFD->s3File = 1;
      pFD->pFD = (TdFilePtr)&pFD->s3File;
      int32_t vid = 0;
      sscanf(object_name, "v%df%dver%" PRId64 ".data", &vid, &pFD->fid, &pFD->cid);
      pFD->objName = object_name;
      // pFD->szFile = s3_size;
#endif
    } else {
      tsdbInfo("no file: %s", path);
      code = TAOS_SYSTEM_ERROR(errsv);
      // taosMemoryFree(pFD);
      goto _exit;
    }
  }

  pFD->pBuf = taosMemoryCalloc(1, szPage);
  if (pFD->pBuf == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    // taosCloseFile(&pFD->pFD);
    // taosMemoryFree(pFD);
    goto _exit;
  }

  // not check file size when reading data files.
  if (flag != TD_FILE_READ && !pFD->s3File) {
    if (taosStatFile(path, &pFD->szFile, NULL, NULL) < 0) {
      code = TAOS_SYSTEM_ERROR(errno);
      // taosMemoryFree(pFD->pBuf);
      // taosCloseFile(&pFD->pFD);
      // taosMemoryFree(pFD);
      goto _exit;
    }

    ASSERT(pFD->szFile % szPage == 0);
    pFD->szFile = pFD->szFile / szPage;
  }

_exit:
  return code;
}

// =============== PAGE-WISE FILE ===============
int32_t tsdbOpenFile(const char *path, STsdb *pTsdb, int32_t flag, STsdbFD **ppFD) {
  int32_t  code = 0;
  STsdbFD *pFD = NULL;
  int32_t  szPage = pTsdb->pVnode->config.tsdbPageSize;

  *ppFD = NULL;

  pFD = (STsdbFD *)taosMemoryCalloc(1, sizeof(*pFD) + strlen(path) + 1);
  if (pFD == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  pFD->path = (char *)&pFD[1];
  strcpy(pFD->path, path);
  pFD->szPage = szPage;
  pFD->flag = flag;
  pFD->szPage = szPage;
  pFD->pgno = 0;
  pFD->pTsdb = pTsdb;

  *ppFD = pFD;

_exit:
  return code;
}

void tsdbCloseFile(STsdbFD **ppFD) {
  STsdbFD *pFD = *ppFD;
  if (pFD) {
    taosMemoryFree(pFD->pBuf);
    if (!pFD->s3File) {
      taosCloseFile(&pFD->pFD);
    }
    taosMemoryFree(pFD);
    *ppFD = NULL;
  }
}

static int32_t tsdbWriteFilePage(STsdbFD *pFD) {
  int32_t code = 0;

  if (!pFD->pFD) {
    code = tsdbOpenFileImpl(pFD);
    if (code) {
      goto _exit;
    }
  }

  if (pFD->s3File) {
    tsdbWarn("%s file: %s", __func__, pFD->path);
    return code;
  }
  if (pFD->pgno > 0) {
    int64_t n = taosLSeekFile(pFD->pFD, PAGE_OFFSET(pFD->pgno, pFD->szPage), SEEK_SET);
    if (n < 0) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _exit;
    }

    taosCalcChecksumAppend(0, pFD->pBuf, pFD->szPage);

    n = taosWriteFile(pFD->pFD, pFD->pBuf, pFD->szPage);
    if (n < 0) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _exit;
    }

    if (pFD->szFile < pFD->pgno) {
      pFD->szFile = pFD->pgno;
    }
  }
  pFD->pgno = 0;

_exit:
  return code;
}

static int32_t tsdbReadFilePage(STsdbFD *pFD, int64_t pgno) {
  int32_t code = 0;

  // ASSERT(pgno <= pFD->szFile);
  if (!pFD->pFD) {
    code = tsdbOpenFileImpl(pFD);
    if (code) {
      goto _exit;
    }
  }

  int64_t offset = PAGE_OFFSET(pgno, pFD->szPage);

  if (pFD->s3File) {
    LRUHandle *handle = NULL;

    pFD->blkno = (pgno + tsS3BlockSize - 1) / tsS3BlockSize;
    code = tsdbCacheGetBlockS3(pFD->pTsdb->bCache, pFD, &handle);
    if (code != TSDB_CODE_SUCCESS || handle == NULL) {
      tsdbCacheRelease(pFD->pTsdb->bCache, handle);
      if (code == TSDB_CODE_SUCCESS && !handle) {
        code = TSDB_CODE_OUT_OF_MEMORY;
      }
      goto _exit;
    }

    uint8_t *pBlock = (uint8_t *)taosLRUCacheValue(pFD->pTsdb->bCache, handle);

    int64_t blk_offset = (pFD->blkno - 1) * tsS3BlockSize * pFD->szPage;
    memcpy(pFD->pBuf, pBlock + (offset - blk_offset), pFD->szPage);

    tsdbCacheRelease(pFD->pTsdb->bCache, handle);
  } else {
    // seek
    int64_t n = taosLSeekFile(pFD->pFD, offset, SEEK_SET);
    if (n < 0) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _exit;
    }

    // read
    n = taosReadFile(pFD->pFD, pFD->pBuf, pFD->szPage);
    if (n < 0) {
      code = TAOS_SYSTEM_ERROR(errno);
      goto _exit;
    } else if (n < pFD->szPage) {
      code = TSDB_CODE_FILE_CORRUPTED;
      goto _exit;
    }
  }

  // check
  if (pgno > 1 && !taosCheckChecksumWhole(pFD->pBuf, pFD->szPage)) {
    code = TSDB_CODE_FILE_CORRUPTED;
    goto _exit;
  }

  pFD->pgno = pgno;

_exit:
  return code;
}

int32_t tsdbWriteFile(STsdbFD *pFD, int64_t offset, const uint8_t *pBuf, int64_t size) {
  int32_t code = 0;
  int64_t fOffset = LOGIC_TO_FILE_OFFSET(offset, pFD->szPage);
  int64_t pgno = OFFSET_PGNO(fOffset, pFD->szPage);
  int64_t bOffset = fOffset % pFD->szPage;
  int64_t n = 0;

  do {
    if (pFD->pgno != pgno) {
      code = tsdbWriteFilePage(pFD);
      if (code) goto _exit;

      if (pgno <= pFD->szFile) {
        code = tsdbReadFilePage(pFD, pgno);
        if (code) goto _exit;
      } else {
        pFD->pgno = pgno;
      }
    }

    int64_t nWrite = TMIN(PAGE_CONTENT_SIZE(pFD->szPage) - bOffset, size - n);
    memcpy(pFD->pBuf + bOffset, pBuf + n, nWrite);

    pgno++;
    bOffset = 0;
    n += nWrite;
  } while (n < size);

_exit:
  return code;
}

static int32_t tsdbReadFileImp(STsdbFD *pFD, int64_t offset, uint8_t *pBuf, int64_t size) {
  int32_t code = 0;
  int64_t n = 0;
  int64_t fOffset = LOGIC_TO_FILE_OFFSET(offset, pFD->szPage);
  int64_t pgno = OFFSET_PGNO(fOffset, pFD->szPage);
  int32_t szPgCont = PAGE_CONTENT_SIZE(pFD->szPage);
  int64_t bOffset = fOffset % pFD->szPage;

  // ASSERT(pgno && pgno <= pFD->szFile);
  ASSERT(bOffset < szPgCont);

  while (n < size) {
    if (pFD->pgno != pgno) {
      code = tsdbReadFilePage(pFD, pgno);
      if (code) goto _exit;
    }

    int64_t nRead = TMIN(szPgCont - bOffset, size - n);
    memcpy(pBuf + n, pFD->pBuf + bOffset, nRead);

    n += nRead;
    pgno++;
    bOffset = 0;
  }

_exit:
  return code;
}

static int32_t tsdbReadFileS3(STsdbFD *pFD, int64_t offset, uint8_t *pBuf, int64_t size, int64_t szHint) {
  int32_t code = 0;
  int64_t n = 0;
  int32_t szPgCont = PAGE_CONTENT_SIZE(pFD->szPage);
  int64_t fOffset = LOGIC_TO_FILE_OFFSET(offset, pFD->szPage);
  int64_t pgno = OFFSET_PGNO(fOffset, pFD->szPage);
  int64_t bOffset = fOffset % pFD->szPage;

  ASSERT(bOffset < szPgCont);

  // 1, find pgnoStart & pgnoEnd to fetch from s3, if all pgs are local, no need to fetch
  // 2, fetch pgnoStart ~ pgnoEnd from s3
  // 3, store pgs to pcache & last pg to pFD->pBuf
  // 4, deliver pgs to [pBuf, pBuf + size)

  while (n < size) {
    if (pFD->pgno != pgno) {
      LRUHandle *handle = NULL;
      code = tsdbCacheGetPageS3(pFD->pTsdb->pgCache, pFD, pgno, &handle);
      if (code != TSDB_CODE_SUCCESS) {
        if (handle) {
          tsdbCacheRelease(pFD->pTsdb->pgCache, handle);
        }
        goto _exit;
      }

      if (!handle) {
        break;
      }

      uint8_t *pPage = (uint8_t *)taosLRUCacheValue(pFD->pTsdb->pgCache, handle);
      memcpy(pFD->pBuf, pPage, pFD->szPage);
      tsdbCacheRelease(pFD->pTsdb->pgCache, handle);

      // check
      if (pgno > 1 && !taosCheckChecksumWhole(pFD->pBuf, pFD->szPage)) {
        code = TSDB_CODE_FILE_CORRUPTED;
        goto _exit;
      }

      pFD->pgno = pgno;
    }

    int64_t nRead = TMIN(szPgCont - bOffset, size - n);
    memcpy(pBuf + n, pFD->pBuf + bOffset, nRead);

    n += nRead;
    ++pgno;
    bOffset = 0;
  }

  if (n < size) {
    // 2, retrieve pgs from s3
    uint8_t *pBlock = NULL;
    int64_t  retrieve_offset = PAGE_OFFSET(pgno, pFD->szPage);
    int64_t  pgnoEnd = pgno - 1 + (bOffset + size - n + szPgCont - 1) / szPgCont;

    if (szHint > 0) {
      pgnoEnd = pgno - 1 + (bOffset + szHint - n + szPgCont - 1) / szPgCont;
    }

    int64_t retrieve_size = (pgnoEnd - pgno + 1) * pFD->szPage;
    code = s3GetObjectBlock(pFD->objName, retrieve_offset, retrieve_size, 1, &pBlock);
    if (code != TSDB_CODE_SUCCESS) {
      goto _exit;
    }

    // 3, Store Pages in Cache
    int nPage = pgnoEnd - pgno + 1;
    for (int i = 0; i < nPage; ++i) {
      tsdbCacheSetPageS3(pFD->pTsdb->pgCache, pFD, pgno, pBlock + i * pFD->szPage);

      if (szHint > 0 && n >= size) {
        ++pgno;
        continue;
      }
      memcpy(pFD->pBuf, pBlock + i * pFD->szPage, pFD->szPage);

      // check
      if (pgno > 1 && !taosCheckChecksumWhole(pFD->pBuf, pFD->szPage)) {
        code = TSDB_CODE_FILE_CORRUPTED;
        goto _exit;
      }

      pFD->pgno = pgno;

      int64_t nRead = TMIN(szPgCont - bOffset, size - n);
      memcpy(pBuf + n, pFD->pBuf + bOffset, nRead);

      n += nRead;
      ++pgno;
      bOffset = 0;
    }

    taosMemoryFree(pBlock);
  }

_exit:
  return code;
}

int32_t tsdbReadFile(STsdbFD *pFD, int64_t offset, uint8_t *pBuf, int64_t size, int64_t szHint) {
  int32_t code = 0;
  if (!pFD->pFD) {
    code = tsdbOpenFileImpl(pFD);
    if (code) {
      goto _exit;
    }
  }

  if (pFD->s3File && tsS3BlockSize < 0) {
    return tsdbReadFileS3(pFD, offset, pBuf, size, szHint);
  } else {
    return tsdbReadFileImp(pFD, offset, pBuf, size);
  }

_exit:
  return code;
}

int32_t tsdbReadFileToBuffer(STsdbFD *pFD, int64_t offset, int64_t size, SBuffer *buffer, int64_t szHint) {
  int32_t code;

  code = tBufferEnsureCapacity(buffer, buffer->size + size);
  if (code) return code;
  code = tsdbReadFile(pFD, offset, (uint8_t *)tBufferGetDataEnd(buffer), size, szHint);
  if (code) return code;
  buffer->size += size;

  return code;
}

int32_t tsdbFsyncFile(STsdbFD *pFD) {
  int32_t code = 0;

  if (pFD->s3File) {
    tsdbWarn("%s file: %s", __func__, pFD->path);
    return code;
  }
  code = tsdbWriteFilePage(pFD);
  if (code) goto _exit;

  if (taosFsyncFile(pFD->pFD) < 0) {
    code = TAOS_SYSTEM_ERROR(errno);
    goto _exit;
  }

_exit:
  return code;
}

// SDataFReader ====================================================
int32_t tsdbDataFReaderOpen(SDataFReader **ppReader, STsdb *pTsdb, SDFileSet *pSet) {
  int32_t       code = 0;
  int32_t       lino = 0;
  SDataFReader *pReader = NULL;
  int32_t       szPage = pTsdb->pVnode->config.tsdbPageSize;
  char          fname[TSDB_FILENAME_LEN];

  // alloc
  pReader = (SDataFReader *)taosMemoryCalloc(1, sizeof(*pReader));
  if (pReader == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    TSDB_CHECK_CODE(code, lino, _exit);
  }
  pReader->pTsdb = pTsdb;
  pReader->pSet = pSet;

  // head
  tsdbHeadFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pHeadF, fname);
  code = tsdbOpenFile(fname, pTsdb, TD_FILE_READ, &pReader->pHeadFD);
  TSDB_CHECK_CODE(code, lino, _exit);

  // data
  tsdbDataFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pDataF, fname);
  code = tsdbOpenFile(fname, pTsdb, TD_FILE_READ, &pReader->pDataFD);
  TSDB_CHECK_CODE(code, lino, _exit);

  // sma
  tsdbSmaFileName(pTsdb, pSet->diskId, pSet->fid, pSet->pSmaF, fname);
  code = tsdbOpenFile(fname, pTsdb, TD_FILE_READ, &pReader->pSmaFD);
  TSDB_CHECK_CODE(code, lino, _exit);

  // stt
  for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
    tsdbSttFileName(pTsdb, pSet->diskId, pSet->fid, pSet->aSttF[iStt], fname);
    code = tsdbOpenFile(fname, pTsdb, TD_FILE_READ, &pReader->aSttFD[iStt]);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

_exit:
  if (code) {
    *ppReader = NULL;
    tsdbError("vgId:%d, %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));

    if (pReader) {
      for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) tsdbCloseFile(&pReader->aSttFD[iStt]);
      tsdbCloseFile(&pReader->pSmaFD);
      tsdbCloseFile(&pReader->pDataFD);
      tsdbCloseFile(&pReader->pHeadFD);
      taosMemoryFree(pReader);
    }
  } else {
    *ppReader = pReader;
  }
  return code;
}

int32_t tsdbDataFReaderClose(SDataFReader **ppReader) {
  int32_t code = 0;
  if (*ppReader == NULL) return code;

  // head
  tsdbCloseFile(&(*ppReader)->pHeadFD);

  // data
  tsdbCloseFile(&(*ppReader)->pDataFD);

  // sma
  tsdbCloseFile(&(*ppReader)->pSmaFD);

  // stt
  for (int32_t iStt = 0; iStt < TSDB_STT_TRIGGER_ARRAY_SIZE; iStt++) {
    if ((*ppReader)->aSttFD[iStt]) {
      tsdbCloseFile(&(*ppReader)->aSttFD[iStt]);
    }
  }

  for (int32_t iBuf = 0; iBuf < sizeof((*ppReader)->aBuf) / sizeof(uint8_t *); iBuf++) {
    tFree((*ppReader)->aBuf[iBuf]);
  }
  taosMemoryFree(*ppReader);
  *ppReader = NULL;
  return code;
}

int32_t tsdbReadBlockIdx(SDataFReader *pReader, SArray *aBlockIdx) {
  int32_t    code = 0;
  SHeadFile *pHeadFile = pReader->pSet->pHeadF;
  int64_t    offset = pHeadFile->offset;
  int64_t    size = pHeadFile->size - offset;

  taosArrayClear(aBlockIdx);
  if (size == 0) return code;

  // alloc
  code = tRealloc(&pReader->aBuf[0], size);
  if (code) goto _err;

  // read
  code = tsdbReadFile(pReader->pHeadFD, offset, pReader->aBuf[0], size, 0);
  if (code) goto _err;

  // decode
  int64_t n = 0;
  while (n < size) {
    SBlockIdx blockIdx;
    n += tGetBlockIdx(pReader->aBuf[0] + n, &blockIdx);

    if (taosArrayPush(aBlockIdx, &blockIdx) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }
  ASSERT(n == size);

  return code;

_err:
  tsdbError("vgId:%d, read block idx failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbReadSttBlk(SDataFReader *pReader, int32_t iStt, SArray *aSttBlk) {
  int32_t   code = 0;
  SSttFile *pSttFile = pReader->pSet->aSttF[iStt];
  int64_t   offset = pSttFile->offset;
  int64_t   size = pSttFile->size - offset;

  taosArrayClear(aSttBlk);
  if (size == 0) return code;

  // alloc
  code = tRealloc(&pReader->aBuf[0], size);
  if (code) goto _err;

  // read
  code = tsdbReadFile(pReader->aSttFD[iStt], offset, pReader->aBuf[0], size, 0);
  if (code) goto _err;

  // decode
  int64_t n = 0;
  while (n < size) {
    SSttBlk sttBlk;
    n += tGetSttBlk(pReader->aBuf[0] + n, &sttBlk);

    if (taosArrayPush(aSttBlk, &sttBlk) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }
  ASSERT(n == size);

  return code;

_err:
  tsdbError("vgId:%d, read stt blk failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbReadDataBlk(SDataFReader *pReader, SBlockIdx *pBlockIdx, SMapData *mDataBlk) {
  int32_t code = 0;
  int64_t offset = pBlockIdx->offset;
  int64_t size = pBlockIdx->size;

  // alloc
  code = tRealloc(&pReader->aBuf[0], size);
  if (code) goto _err;

  // read
  code = tsdbReadFile(pReader->pHeadFD, offset, pReader->aBuf[0], size, 0);
  if (code) goto _err;

  // decode
  int64_t n = tGetMapData(pReader->aBuf[0], mDataBlk);
  if (n < 0) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  ASSERT(n == size);

  return code;

_err:
  tsdbError("vgId:%d, read block failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}

// SDelFReader ====================================================
struct SDelFReader {
  STsdb   *pTsdb;
  SDelFile fDel;
  STsdbFD *pReadH;
  uint8_t *aBuf[1];
};

int32_t tsdbDelFReaderOpen(SDelFReader **ppReader, SDelFile *pFile, STsdb *pTsdb) {
  int32_t      code = 0;
  int32_t      lino = 0;
  char         fname[TSDB_FILENAME_LEN];
  SDelFReader *pDelFReader = NULL;

  // alloc
  pDelFReader = (SDelFReader *)taosMemoryCalloc(1, sizeof(*pDelFReader));
  if (pDelFReader == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  // open impl
  pDelFReader->pTsdb = pTsdb;
  pDelFReader->fDel = *pFile;

  tsdbDelFileName(pTsdb, pFile, fname);
  code = tsdbOpenFile(fname, pTsdb, TD_FILE_READ, &pDelFReader->pReadH);
  if (code) {
    taosMemoryFree(pDelFReader);
    goto _exit;
  }

_exit:
  if (code) {
    *ppReader = NULL;
    tsdbError("vgId:%d, %s failed at %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  } else {
    *ppReader = pDelFReader;
  }
  return code;
}

int32_t tsdbDelFReaderClose(SDelFReader **ppReader) {
  int32_t      code = 0;
  SDelFReader *pReader = *ppReader;

  if (pReader) {
    tsdbCloseFile(&pReader->pReadH);
    for (int32_t iBuf = 0; iBuf < sizeof(pReader->aBuf) / sizeof(uint8_t *); iBuf++) {
      tFree(pReader->aBuf[iBuf]);
    }
    taosMemoryFree(pReader);
  }

  *ppReader = NULL;
  return code;
}

int32_t tsdbReadDelData(SDelFReader *pReader, SDelIdx *pDelIdx, SArray *aDelData) {
  return tsdbReadDelDatav1(pReader, pDelIdx, aDelData, INT64_MAX);
}

int32_t tsdbReadDelDatav1(SDelFReader *pReader, SDelIdx *pDelIdx, SArray *aDelData, int64_t maxVer) {
  int32_t code = 0;
  int64_t offset = pDelIdx->offset;
  int64_t size = pDelIdx->size;
  int64_t n;

  taosArrayClear(aDelData);

  // alloc
  code = tRealloc(&pReader->aBuf[0], size);
  if (code) goto _err;

  // read
  code = tsdbReadFile(pReader->pReadH, offset, pReader->aBuf[0], size, 0);
  if (code) goto _err;

  // // decode
  n = 0;
  while (n < size) {
    SDelData delData;
    n += tGetDelData(pReader->aBuf[0] + n, &delData);

    if (delData.version > maxVer) {
      continue;
    }
    if (taosArrayPush(aDelData, &delData) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }

  ASSERT(n == size);

  return code;

_err:
  tsdbError("vgId:%d, read del data failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbReadDelIdx(SDelFReader *pReader, SArray *aDelIdx) {
  int32_t code = 0;
  int32_t n;
  int64_t offset = pReader->fDel.offset;
  int64_t size = pReader->fDel.size - offset;

  taosArrayClear(aDelIdx);

  // alloc
  code = tRealloc(&pReader->aBuf[0], size);
  if (code) goto _err;

  // read
  code = tsdbReadFile(pReader->pReadH, offset, pReader->aBuf[0], size, 0);
  if (code) goto _err;

  // decode
  n = 0;
  while (n < size) {
    SDelIdx delIdx;

    n += tGetDelIdx(pReader->aBuf[0] + n, &delIdx);

    if (taosArrayPush(aDelIdx, &delIdx) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }

  ASSERT(n == size);

  return code;

_err:
  tsdbError("vgId:%d, read del idx failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}
