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

#include "meta.h"

extern SDmNotifyHandle dmNotifyHdl;

int32_t metaAddTableColumn(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp);
int32_t metaDropTableColumn(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp);
int32_t metaAlterTableColumnName(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp);
int32_t metaAlterTableColumnBytes(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp);
int32_t metaUpdateTableTagValue(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq);
int32_t metaUpdateTableMultiTagValue(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq);
int32_t metaUpdateTableOptions2(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq);

int32_t metaSaveJsonVarToIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry, const SSchema *pSchema);

int32_t     metaDelJsonVarFromIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry, const SSchema *pSchema);
static int  metaSaveToTbDb(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateUidIdx(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateNameIdx(SMeta *pMeta, const SMetaEntry *pME);
static void metaUpdateTtl(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateChangeTime(SMeta *pMeta, tb_uid_t uid, int64_t changeTimeMs);
static int  metaSaveToSkmDb(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateCtbIdx(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateSuidIdx(SMeta *pMeta, const SMetaEntry *pME);
static int  metaUpdateTagIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry);
static int  metaDropTableByUid(SMeta *pMeta, tb_uid_t uid, int *type, tb_uid_t *pSuid, int8_t *pSysTbl);
void        metaDestroyTagIdxKey(STagIdxKey *pTagIdxKey);
// opt ins_tables query
static int metaUpdateBtimeIdx(SMeta *pMeta, const SMetaEntry *pME);
static int metaDeleteBtimeIdx(SMeta *pMeta, const SMetaEntry *pME);
static int metaUpdateNcolIdx(SMeta *pMeta, const SMetaEntry *pME);
static int metaDeleteNcolIdx(SMeta *pMeta, const SMetaEntry *pME);

int32_t updataTableColCmpr(SColCmprWrapper *pWp, SSchema *pSchema, int8_t add, uint32_t compress) {
  int32_t nCols = pWp->nCols;
  int32_t ver = pWp->version;
  if (add) {
    SColCmpr *p = taosMemoryRealloc(pWp->pColCmpr, sizeof(SColCmpr) * (nCols + 1));
    if (p == NULL) {
      return terrno;
    }
    pWp->pColCmpr = p;

    SColCmpr *pCol = p + nCols;
    pCol->id = pSchema->colId;
    pCol->alg = compress;
    pWp->nCols = nCols + 1;
    pWp->version = ver;
  } else {
    for (int32_t i = 0; i < nCols; i++) {
      SColCmpr *pOCmpr = &pWp->pColCmpr[i];
      if (pOCmpr->id == pSchema->colId) {
        int32_t left = (nCols - i - 1) * sizeof(SColCmpr);
        if (left) {
          memmove(pWp->pColCmpr + i, pWp->pColCmpr + i + 1, left);
        }
        nCols--;
        break;
      }
    }
    pWp->nCols = nCols;
    pWp->version = ver;
  }
  return 0;
}
static void metaGetEntryInfo(const SMetaEntry *pEntry, SMetaInfo *pInfo) {
  pInfo->uid = pEntry->uid;
  pInfo->version = pEntry->version;
  if (pEntry->type == TSDB_SUPER_TABLE) {
    pInfo->suid = pEntry->uid;
    pInfo->skmVer = pEntry->stbEntry.schemaRow.version;
  } else if (pEntry->type == TSDB_CHILD_TABLE) {
    pInfo->suid = pEntry->ctbEntry.suid;
    pInfo->skmVer = 0;
  } else if (pEntry->type == TSDB_NORMAL_TABLE) {
    pInfo->suid = 0;
    pInfo->skmVer = pEntry->ntbEntry.schemaRow.version;
  } else {
    metaError("meta/table: invalide table type: %" PRId8 " get entry info failed.", pEntry->type);
  }
}

int metaUpdateMetaRsp(tb_uid_t uid, char *tbName, SSchemaWrapper *pSchema, STableMetaRsp *pMetaRsp) {
  pMetaRsp->pSchemas = taosMemoryMalloc(pSchema->nCols * sizeof(SSchema));
  if (NULL == pMetaRsp->pSchemas) {
    return terrno;
  }

  pMetaRsp->pSchemaExt = taosMemoryMalloc(pSchema->nCols * sizeof(SSchemaExt));
  if (pMetaRsp->pSchemaExt == NULL) {
    taosMemoryFree(pMetaRsp->pSchemas);
    return terrno;
  }

  tstrncpy(pMetaRsp->tbName, tbName, TSDB_TABLE_NAME_LEN);
  pMetaRsp->numOfColumns = pSchema->nCols;
  pMetaRsp->tableType = TSDB_NORMAL_TABLE;
  pMetaRsp->sversion = pSchema->version;
  pMetaRsp->tuid = uid;

  memcpy(pMetaRsp->pSchemas, pSchema->pSchema, pSchema->nCols * sizeof(SSchema));

  return 0;
}

int metaSaveJsonVarToIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry, const SSchema *pSchema) {
  int32_t code = 0;

#ifdef USE_INVERTED_INDEX
  if (pMeta->pTagIvtIdx == NULL || pCtbEntry == NULL) {
    return TSDB_CODE_INVALID_PARA;
  }
  void       *data = pCtbEntry->ctbEntry.pTags;
  const char *tagName = pSchema->name;

  tb_uid_t    suid = pCtbEntry->ctbEntry.suid;
  tb_uid_t    tuid = pCtbEntry->uid;
  const void *pTagData = pCtbEntry->ctbEntry.pTags;
  int32_t     nTagData = 0;

  SArray *pTagVals = NULL;
  code = tTagToValArray((const STag *)data, &pTagVals);
  if (code) {
    return code;
  }

  SIndexMultiTerm *terms = indexMultiTermCreate();
  if (terms == NULL) {
    return terrno;
  }

  int16_t nCols = taosArrayGetSize(pTagVals);
  for (int i = 0; i < nCols; i++) {
    STagVal *pTagVal = (STagVal *)taosArrayGet(pTagVals, i);
    char     type = pTagVal->type;

    char   *key = pTagVal->pKey;
    int32_t nKey = strlen(key);

    SIndexTerm *term = NULL;
    if (type == TSDB_DATA_TYPE_NULL) {
      term = indexTermCreate(suid, ADD_VALUE, TSDB_DATA_TYPE_VARCHAR, key, nKey, NULL, 0);
    } else if (type == TSDB_DATA_TYPE_NCHAR) {
      if (pTagVal->nData > 0) {
        char *val = taosMemoryCalloc(1, pTagVal->nData + VARSTR_HEADER_SIZE);
        if (val == NULL) {
          TAOS_CHECK_GOTO(terrno, NULL, _exception);
        }
        int32_t len = taosUcs4ToMbs((TdUcs4 *)pTagVal->pData, pTagVal->nData, val + VARSTR_HEADER_SIZE, NULL);
        if (len < 0) {
          TAOS_CHECK_GOTO(len, NULL, _exception);
        }
        memcpy(val, (uint16_t *)&len, VARSTR_HEADER_SIZE);
        type = TSDB_DATA_TYPE_VARCHAR;
        term = indexTermCreate(suid, ADD_VALUE, type, key, nKey, val, len);
        taosMemoryFree(val);
      } else if (pTagVal->nData == 0) {
        term = indexTermCreate(suid, ADD_VALUE, TSDB_DATA_TYPE_VARCHAR, key, nKey, pTagVal->pData, 0);
      }
    } else if (type == TSDB_DATA_TYPE_DOUBLE) {
      double val = *(double *)(&pTagVal->i64);
      int    len = sizeof(val);
      term = indexTermCreate(suid, ADD_VALUE, type, key, nKey, (const char *)&val, len);
    } else if (type == TSDB_DATA_TYPE_BOOL) {
      int val = *(int *)(&pTagVal->i64);
      int len = sizeof(val);
      term = indexTermCreate(suid, ADD_VALUE, TSDB_DATA_TYPE_BOOL, key, nKey, (const char *)&val, len);
    }

    if (term != NULL) {
      int32_t ret = indexMultiTermAdd(terms, term);
      if (ret < 0) {
        metaError("vgId:%d, failed to add term to multi term, uid: %" PRId64 ", key: %s, type: %d, ret: %d",
                  TD_VID(pMeta->pVnode), tuid, key, type, ret);
      }
    } else {
      code = terrno;
      goto _exception;
    }
  }
  code = indexJsonPut(pMeta->pTagIvtIdx, terms, tuid);
  indexMultiTermDestroy(terms);

  taosArrayDestroy(pTagVals);
#endif
  return code;
_exception:
  indexMultiTermDestroy(terms);
  taosArrayDestroy(pTagVals);
  return code;
}
int metaDelJsonVarFromIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry, const SSchema *pSchema) {
#ifdef USE_INVERTED_INDEX
  if (pMeta->pTagIvtIdx == NULL || pCtbEntry == NULL) {
    return TSDB_CODE_INVALID_PARA;
  }
  void       *data = pCtbEntry->ctbEntry.pTags;
  const char *tagName = pSchema->name;

  tb_uid_t    suid = pCtbEntry->ctbEntry.suid;
  tb_uid_t    tuid = pCtbEntry->uid;
  const void *pTagData = pCtbEntry->ctbEntry.pTags;
  int32_t     nTagData = 0;

  SArray *pTagVals = NULL;
  int32_t code = tTagToValArray((const STag *)data, &pTagVals);
  if (code) {
    return code;
  }

  SIndexMultiTerm *terms = indexMultiTermCreate();
  if (terms == NULL) {
    return terrno;
  }

  int16_t nCols = taosArrayGetSize(pTagVals);
  for (int i = 0; i < nCols; i++) {
    STagVal *pTagVal = (STagVal *)taosArrayGet(pTagVals, i);
    char     type = pTagVal->type;

    char   *key = pTagVal->pKey;
    int32_t nKey = strlen(key);

    SIndexTerm *term = NULL;
    if (type == TSDB_DATA_TYPE_NULL) {
      term = indexTermCreate(suid, DEL_VALUE, TSDB_DATA_TYPE_VARCHAR, key, nKey, NULL, 0);
    } else if (type == TSDB_DATA_TYPE_NCHAR) {
      if (pTagVal->nData > 0) {
        char *val = taosMemoryCalloc(1, pTagVal->nData + VARSTR_HEADER_SIZE);
        if (val == NULL) {
          TAOS_CHECK_GOTO(terrno, NULL, _exception);
        }
        int32_t len = taosUcs4ToMbs((TdUcs4 *)pTagVal->pData, pTagVal->nData, val + VARSTR_HEADER_SIZE, NULL);
        if (len < 0) {
          TAOS_CHECK_GOTO(len, NULL, _exception);
        }
        memcpy(val, (uint16_t *)&len, VARSTR_HEADER_SIZE);
        type = TSDB_DATA_TYPE_VARCHAR;
        term = indexTermCreate(suid, DEL_VALUE, type, key, nKey, val, len);
        taosMemoryFree(val);
      } else if (pTagVal->nData == 0) {
        term = indexTermCreate(suid, DEL_VALUE, TSDB_DATA_TYPE_VARCHAR, key, nKey, pTagVal->pData, 0);
      }
    } else if (type == TSDB_DATA_TYPE_DOUBLE) {
      double val = *(double *)(&pTagVal->i64);
      int    len = sizeof(val);
      term = indexTermCreate(suid, DEL_VALUE, type, key, nKey, (const char *)&val, len);
    } else if (type == TSDB_DATA_TYPE_BOOL) {
      int val = *(int *)(&pTagVal->i64);
      int len = sizeof(val);
      term = indexTermCreate(suid, DEL_VALUE, TSDB_DATA_TYPE_BOOL, key, nKey, (const char *)&val, len);
    }
    if (term != NULL) {
      int32_t ret = indexMultiTermAdd(terms, term);
      if (ret < 0) {
        metaError("vgId:%d, failed to add term to multi term, uid: %" PRId64 ", key: %s, type: %d, ret: %d",
                  TD_VID(pMeta->pVnode), tuid, key, type, ret);
      }
    } else {
      code = terrno;
      goto _exception;
    }
  }
  code = indexJsonPut(pMeta->pTagIvtIdx, terms, tuid);
  indexMultiTermDestroy(terms);
  taosArrayDestroy(pTagVals);
#endif
  return code;
_exception:
  indexMultiTermDestroy(terms);
  taosArrayDestroy(pTagVals);
  return code;
}

void metaTimeSeriesNotifyCheck(SMeta *pMeta) {
#if defined(TD_ENTERPRISE)
  int64_t nTimeSeries = metaGetTimeSeriesNum(pMeta, 0);
  int64_t deltaTS = nTimeSeries - pMeta->pVnode->config.vndStats.numOfReportedTimeSeries;
  if (deltaTS > tsTimeSeriesThreshold) {
    if (0 == atomic_val_compare_exchange_8(&dmNotifyHdl.state, 1, 2)) {
      if (tsem_post(&dmNotifyHdl.sem) != 0) {
        metaError("vgId:%d, failed to post semaphore, errno:%d", TD_VID(pMeta->pVnode), errno);
      }
    }
  }
#endif
}

int metaCreateSTable(SMeta *pMeta, int64_t version, SVCreateStbReq *pReq) {
  SMetaEntry  me = {0};
  int         kLen = 0;
  int         vLen = 0;
  const void *pKey = NULL;
  const void *pVal = NULL;
  void       *pBuf = NULL;
  int32_t     szBuf = 0;
  void       *p = NULL;
  int32_t     code = 0;

  // validate req
  void *pData = NULL;
  int   nData = 0;
  if (tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &pData, &nData) == 0) {
    tb_uid_t uid = *(tb_uid_t *)pData;
    tdbFree(pData);
    SMetaInfo info;
    if (metaGetInfo(pMeta, uid, &info, NULL) == TSDB_CODE_NOT_FOUND) {
      return terrno = TSDB_CODE_PAR_TABLE_NOT_EXIST;
    }
    if (info.uid == info.suid) {
      return 0;
    } else {
      return terrno = TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
    }
  }

  // set structs
  me.version = version;
  me.type = TSDB_SUPER_TABLE;
  me.uid = pReq->suid;
  me.name = pReq->name;
  me.stbEntry.schemaRow = pReq->schemaRow;
  me.stbEntry.schemaTag = pReq->schemaTag;
  if (pReq->rollup) {
    TABLE_SET_ROLLUP(me.flags);
    me.stbEntry.rsmaParam = pReq->rsmaParam;
  }
  if (pReq->colCmpred) {
    TABLE_SET_COL_COMPRESSED(me.flags);
    me.colCmpr = pReq->colCmpr;
  }

  code = metaHandleEntry(pMeta, &me);
  if (code) goto _err;

  ++pMeta->pVnode->config.vndStats.numOfSTables;

  pMeta->changed = true;
  metaDebug("vgId:%d, stb:%s is created, suid:%" PRId64, TD_VID(pMeta->pVnode), pReq->name, pReq->suid);

  return 0;

_err:
  metaError("vgId:%d, failed to create stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
            tstrerror(terrno));
  return code;
}

int metaDropSTable(SMeta *pMeta, int64_t verison, SVDropStbReq *pReq, SArray *tbUidList) {
  void   *pKey = NULL;
  int     nKey = 0;
  void   *pData = NULL;
  int     nData = 0;
  int     c = 0;
  int     rc = 0;
  int32_t lino;
  int32_t ret;

  // check if super table exists
  rc = tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &pData, &nData);
  if (rc < 0 || *(tb_uid_t *)pData != pReq->suid) {
    tdbFree(pData);
    return terrno = TSDB_CODE_TDB_STB_NOT_EXIST;
  }

  // drop all child tables
  TBC *pCtbIdxc = NULL;

  rc = tdbTbcOpen(pMeta->pCtbIdx, &pCtbIdxc, NULL);
  if (rc) {
    return (terrno = rc);
  }

  rc = tdbTbcMoveTo(pCtbIdxc, &(SCtbIdxKey){.suid = pReq->suid, .uid = INT64_MIN}, sizeof(SCtbIdxKey), &c);
  if (rc < 0) {
    tdbTbcClose(pCtbIdxc);
    metaWLock(pMeta);
    goto _drop_super_table;
  }

  for (;;) {
    rc = tdbTbcNext(pCtbIdxc, &pKey, &nKey, NULL, NULL);
    if (rc < 0) break;

    if (((SCtbIdxKey *)pKey)->suid < pReq->suid) {
      continue;
    } else if (((SCtbIdxKey *)pKey)->suid > pReq->suid) {
      break;
    }

    if (taosArrayPush(tbUidList, &(((SCtbIdxKey *)pKey)->uid)) == NULL) {
      tdbFree(pKey);
      tdbTbcClose(pCtbIdxc);
      return terrno;
    }
  }

  tdbTbcClose(pCtbIdxc);

  ret = tsdbCacheDropSubTables(pMeta->pVnode->pTsdb, tbUidList, pReq->suid);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  metaWLock(pMeta);

  for (int32_t iChild = 0; iChild < taosArrayGetSize(tbUidList); iChild++) {
    tb_uid_t uid = *(tb_uid_t *)taosArrayGet(tbUidList, iChild);
    ret = metaDropTableByUid(pMeta, uid, NULL, NULL, NULL);
    if (ret < 0) {
      metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->suid, tstrerror(terrno));
    }
  }

  // drop super table
_drop_super_table:
  tdbTbGet(pMeta->pUidIdx, &pReq->suid, sizeof(tb_uid_t), &pData, &nData);
  ret = tdbTbDelete(pMeta->pTbDb, &(STbDbKey){.version = ((SUidIdxVal *)pData)[0].version, .uid = pReq->suid},
                    sizeof(STbDbKey), pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  ret = tdbTbDelete(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  ret = metaCacheDrop(pMeta, pReq->suid);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  ret = tdbTbDelete(pMeta->pUidIdx, &pReq->suid, sizeof(tb_uid_t), pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  ret = tdbTbDelete(pMeta->pSuidIdx, &pReq->suid, sizeof(tb_uid_t), pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  ret = metaStatsCacheDrop(pMeta, pReq->suid);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(terrno));
  }

  metaULock(pMeta);

  metaUpdTimeSeriesNum(pMeta);

  pMeta->changed = true;

_exit:
  tdbFree(pKey);
  tdbFree(pData);
  metaDebug("vgId:%d, super table %s uid:%" PRId64 " is dropped", TD_VID(pMeta->pVnode), pReq->name, pReq->suid);
  return 0;
}

static int32_t metaGetSubtables(SMeta *pMeta, int64_t suid, SArray *uids) {
  if (!uids) return TSDB_CODE_INVALID_PARA;

  int   c = 0;
  void *pKey = NULL;
  int   nKey = 0;
  TBC  *pCtbIdxc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pCtbIdx, &pCtbIdxc, NULL));
  int rc = tdbTbcMoveTo(pCtbIdxc, &(SCtbIdxKey){.suid = suid, .uid = INT64_MIN}, sizeof(SCtbIdxKey), &c);
  if (rc < 0) {
    tdbTbcClose(pCtbIdxc);
    metaWLock(pMeta);
    return 0;
  }

  for (;;) {
    rc = tdbTbcNext(pCtbIdxc, &pKey, &nKey, NULL, NULL);
    if (rc < 0) break;

    if (((SCtbIdxKey *)pKey)->suid < suid) {
      continue;
    } else if (((SCtbIdxKey *)pKey)->suid > suid) {
      break;
    }

    if (taosArrayPush(uids, &(((SCtbIdxKey *)pKey)->uid)) == NULL) {
      tdbFree(pKey);
      tdbTbcClose(pCtbIdxc);
      return terrno;
    }
  }

  tdbFree(pKey);

  tdbTbcClose(pCtbIdxc);
  return 0;
}

int metaAlterSTable(SMeta *pMeta, int64_t version, SVCreateStbReq *pReq) {
  SMetaEntry  oStbEntry = {0};
  SMetaEntry  nStbEntry = {0};
  TBC        *pUidIdxc = NULL;
  TBC        *pTbDbc = NULL;
  const void *pData;
  int         nData;
  int64_t     oversion;
  SDecoder    dc = {0};
  int32_t     ret;
  int32_t     c = -2;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pUidIdx, &pUidIdxc, NULL));
  ret = tdbTbcMoveTo(pUidIdxc, &pReq->suid, sizeof(tb_uid_t), &c);
  if (ret < 0 || c) {
    tdbTbcClose(pUidIdxc);

    return terrno = TSDB_CODE_TDB_STB_NOT_EXIST;
  }

  ret = tdbTbcGet(pUidIdxc, NULL, NULL, &pData, &nData);
  if (ret < 0) {
    tdbTbcClose(pUidIdxc);

    return terrno = TSDB_CODE_TDB_STB_NOT_EXIST;
  }

  oversion = ((SUidIdxVal *)pData)[0].version;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTbDb, &pTbDbc, NULL));
  ret = tdbTbcMoveTo(pTbDbc, &((STbDbKey){.uid = pReq->suid, .version = oversion}), sizeof(STbDbKey), &c);
  if (!(ret == 0 && c == 0)) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);

    metaError("meta/table: invalide ret: %" PRId32 " or c: %" PRId32 "alter stb failed.", ret, c);
    return terrno = TSDB_CODE_TDB_STB_NOT_EXIST;
  }

  ret = tdbTbcGet(pTbDbc, NULL, NULL, &pData, &nData);
  if (ret < 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);

    return terrno = TSDB_CODE_TDB_STB_NOT_EXIST;
  }

  if ((oStbEntry.pBuf = taosMemoryMalloc(nData)) == NULL) {
    tdbTbcClose(pTbDbc);
    tdbTbcClose(pUidIdxc);
    return terrno;
  }
  memcpy(oStbEntry.pBuf, pData, nData);
  tDecoderInit(&dc, oStbEntry.pBuf, nData);
  ret = metaDecodeEntry(&dc, &oStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to decode stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(ret));
    tDecoderClear(&dc);
    tdbTbcClose(pTbDbc);
    tdbTbcClose(pUidIdxc);
    return terrno;
  }

  nStbEntry.version = version;
  nStbEntry.type = TSDB_SUPER_TABLE;
  nStbEntry.uid = pReq->suid;
  nStbEntry.name = pReq->name;
  nStbEntry.stbEntry.schemaRow = pReq->schemaRow;
  nStbEntry.stbEntry.schemaTag = pReq->schemaTag;
  nStbEntry.colCmpr = pReq->colCmpr;
  TABLE_SET_COL_COMPRESSED(nStbEntry.flags);

  int     nCols = pReq->schemaRow.nCols;
  int     onCols = oStbEntry.stbEntry.schemaRow.nCols;
  int32_t deltaCol = nCols - onCols;
  bool    updStat = deltaCol != 0 && !metaTbInFilterCache(pMeta, pReq->name, 1);

  if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
    STsdb  *pTsdb = pMeta->pVnode->pTsdb;
    SArray *uids = taosArrayInit(8, sizeof(int64_t));
    if (uids == NULL) {
      if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
      tDecoderClear(&dc);
      tdbTbcClose(pTbDbc);
      tdbTbcClose(pUidIdxc);
      return terrno;
    }
    if (deltaCol == 1) {
      int16_t cid = pReq->schemaRow.pSchema[nCols - 1].colId;
      int8_t  col_type = pReq->schemaRow.pSchema[nCols - 1].type;

      TAOS_CHECK_RETURN(metaGetSubtables(pMeta, pReq->suid, uids));
      TAOS_CHECK_RETURN(tsdbCacheNewSTableColumn(pTsdb, uids, cid, col_type));
    } else if (deltaCol == -1) {
      int16_t cid = -1;
      bool    hasPrimaryKey = false;
      if (onCols >= 2) {
        hasPrimaryKey = (oStbEntry.stbEntry.schemaRow.pSchema[1].flags & COL_IS_KEY) ? true : false;
      }
      for (int i = 0, j = 0; i < nCols && j < onCols; ++i, ++j) {
        if (pReq->schemaRow.pSchema[i].colId != oStbEntry.stbEntry.schemaRow.pSchema[j].colId) {
          cid = oStbEntry.stbEntry.schemaRow.pSchema[j].colId;
          break;
        }
      }

      if (cid != -1) {
        TAOS_CHECK_RETURN(metaGetSubtables(pMeta, pReq->suid, uids));
        TAOS_CHECK_RETURN(tsdbCacheDropSTableColumn(pTsdb, uids, cid, hasPrimaryKey));
      }
    }
    if (uids) taosArrayDestroy(uids);

    tsdbCacheInvalidateSchema(pTsdb, pReq->suid, -1, pReq->schemaRow.version);
  }

  metaWLock(pMeta);
  // compare two entry
  if (oStbEntry.stbEntry.schemaRow.version != pReq->schemaRow.version) {
    ret = metaSaveToSkmDb(pMeta, &nStbEntry);
    if (ret < 0) {
      metaError("vgId:%d, failed to save skm db:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->suid, tstrerror(ret));
    }
  }

  // update table.db
  ret = metaSaveToTbDb(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to save tb db:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(ret));
  }

  // update uid index
  ret = metaUpdateUidIdx(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to update uid idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(ret));
  }

  // metaStatsCacheDrop(pMeta, nStbEntry.uid);

  if (updStat) {
    metaUpdateStbStats(pMeta, pReq->suid, 0, deltaCol);
  }
  metaULock(pMeta);

  if (updStat) {
    int64_t ctbNum;
    ret = metaGetStbStats(pMeta->pVnode, pReq->suid, &ctbNum, NULL);
    if (ret < 0) {
      metaError("vgId:%d, failed to get stb stats:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->suid, tstrerror(ret));
    }
    pMeta->pVnode->config.vndStats.numOfTimeSeries += (ctbNum * deltaCol);
    metaTimeSeriesNotifyCheck(pMeta);
  }

  pMeta->changed = true;

_exit:
  if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
  tDecoderClear(&dc);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  return 0;
}
int metaAddIndexToSTable(SMeta *pMeta, int64_t version, SVCreateStbReq *pReq) {
  SMetaEntry oStbEntry = {0};
  SMetaEntry nStbEntry = {0};
  STbDbKey   tbDbKey = {0};

  TBC     *pUidIdxc = NULL;
  TBC     *pTbDbc = NULL;
  void    *pData = NULL;
  int      nData = 0;
  int64_t  oversion;
  SDecoder dc = {0};
  int32_t  ret;
  int32_t  c = -2;
  tb_uid_t suid = pReq->suid;
  int32_t  code = 0;

  // get super table
  if ((code = tdbTbGet(pMeta->pUidIdx, &suid, sizeof(tb_uid_t), &pData, &nData)) != 0) {
    goto _err;
  }

  tbDbKey.uid = suid;
  tbDbKey.version = ((SUidIdxVal *)pData)[0].version;
  if ((code = tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pData, &nData)) != 0) {
    goto _err;
  }

  tDecoderInit(&dc, pData, nData);
  code = metaDecodeEntry(&dc, &oStbEntry);
  if (code < 0) {
    goto _err;
  }

  if (oStbEntry.stbEntry.schemaTag.pSchema == NULL || oStbEntry.stbEntry.schemaTag.pSchema == NULL) {
    code = TSDB_CODE_INVALID_PARA;
    goto _err;
  }

  if (oStbEntry.stbEntry.schemaTag.version == pReq->schemaTag.version) {
    code = TSDB_CODE_INVALID_PARA;
    goto _err;
  }

  if (oStbEntry.stbEntry.schemaTag.nCols != pReq->schemaTag.nCols) {
    code = TSDB_CODE_INVALID_PARA;
    goto _err;
  }

  int diffIdx = -1;
  for (int i = 0; i < pReq->schemaTag.nCols; i++) {
    SSchema *pNew = pReq->schemaTag.pSchema + i;
    SSchema *pOld = oStbEntry.stbEntry.schemaTag.pSchema + i;
    if (pNew->type != pOld->type || pNew->colId != pOld->colId || pNew->bytes != pOld->bytes ||
        strncmp(pOld->name, pNew->name, sizeof(pNew->name))) {
      code = TSDB_CODE_INVALID_PARA;
      goto _err;
    }
    if (IS_IDX_ON(pNew) && !IS_IDX_ON(pOld)) {
      // if (diffIdx != -1) goto _err;
      diffIdx = i;
      break;
    }
  }

  if (diffIdx == -1) {
    code = TSDB_CODE_INVALID_PARA;
    goto _err;
  }

  // Get target schema info
  SSchemaWrapper *pTagSchema = &pReq->schemaTag;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    code = TSDB_CODE_INVALID_PARA;
    goto _err;
  }
  SSchema *pCol = pTagSchema->pSchema + diffIdx;

  /*
   * iterator all pTdDbc by uid and version
   */
  TBC *pCtbIdxc = NULL;
  code = tdbTbcOpen(pMeta->pCtbIdx, &pCtbIdxc, NULL);
  if (code != 0) {
    goto _err;
  }

  code = tdbTbcMoveTo(pCtbIdxc, &(SCtbIdxKey){.suid = suid, .uid = INT64_MIN}, sizeof(SCtbIdxKey), &c);
  if (code < 0) {
    tdbTbcClose(pCtbIdxc);
    goto _err;
  }
  for (;;) {
    void *pKey = NULL, *pVal = NULL;
    int   nKey = 0, nVal = 0;
    code = tdbTbcNext(pCtbIdxc, &pKey, &nKey, &pVal, &nVal);
    if (code < 0) {
      tdbFree(pKey);
      tdbFree(pVal);
      tdbTbcClose(pCtbIdxc);
      pCtbIdxc = NULL;
      break;
    }
    if (((SCtbIdxKey *)pKey)->suid != suid) {
      tdbFree(pKey);
      tdbFree(pVal);
      continue;
    }
    STagIdxKey *pTagIdxKey = NULL;
    int32_t     nTagIdxKey;

    const void *pTagData = NULL;
    int32_t     nTagData = 0;

    SCtbIdxKey *table = (SCtbIdxKey *)pKey;
    STagVal     tagVal = {.cid = pCol->colId};
    if (tTagGet((const STag *)pVal, &tagVal)) {
      if (IS_VAR_DATA_TYPE(pCol->type)) {
        pTagData = tagVal.pData;
        nTagData = (int32_t)tagVal.nData;
      } else {
        pTagData = &(tagVal.i64);
        nTagData = tDataTypes[pCol->type].bytes;
      }
    } else {
      if (!IS_VAR_DATA_TYPE(pCol->type)) {
        nTagData = tDataTypes[pCol->type].bytes;
      }
    }
    code = metaCreateTagIdxKey(suid, pCol->colId, pTagData, nTagData, pCol->type, table->uid, &pTagIdxKey, &nTagIdxKey);
    tdbFree(pKey);
    tdbFree(pVal);
    if (code < 0) {
      metaDestroyTagIdxKey(pTagIdxKey);
      tdbTbcClose(pCtbIdxc);
      goto _err;
    }

    metaWLock(pMeta);
    ret = tdbTbUpsert(pMeta->pTagIdx, pTagIdxKey, nTagIdxKey, NULL, 0, pMeta->txn);
    if (ret < 0) {
      metaError("vgId:%d, failed to upsert tag idx key:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->suid, tstrerror(ret));
    }
    metaULock(pMeta);

    metaDestroyTagIdxKey(pTagIdxKey);
    pTagIdxKey = NULL;
  }

  nStbEntry.version = version;
  nStbEntry.type = TSDB_SUPER_TABLE;
  nStbEntry.uid = pReq->suid;
  nStbEntry.name = pReq->name;
  nStbEntry.stbEntry.schemaRow = pReq->schemaRow;
  nStbEntry.stbEntry.schemaTag = pReq->schemaTag;
  nStbEntry.colCmpr = pReq->colCmpr;

  metaWLock(pMeta);
  // update table.db
  ret = metaSaveToTbDb(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to save tb db:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(ret));
  }
  // update uid index
  ret = metaUpdateUidIdx(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to update uid idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(ret));
  }
  metaULock(pMeta);

  if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
  tDecoderClear(&dc);
  tdbFree(pData);

  tdbTbcClose(pCtbIdxc);
  return TSDB_CODE_SUCCESS;
_err:
  if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
  tDecoderClear(&dc);
  tdbFree(pData);

  return code;
}
int metaDropIndexFromSTable(SMeta *pMeta, int64_t version, SDropIndexReq *pReq) {
  int32_t    code = 0;
  SMetaEntry oStbEntry = {0};
  SMetaEntry nStbEntry = {0};

  STbDbKey tbDbKey = {0};
  TBC     *pUidIdxc = NULL;
  TBC     *pTbDbc = NULL;
  int      ret = 0;
  int      c = -2;
  void    *pData = NULL;
  int      nData = 0;
  int64_t  oversion;
  SDecoder dc = {0};

  tb_uid_t suid = pReq->stbUid;

  if ((code = tdbTbGet(pMeta->pUidIdx, &suid, sizeof(tb_uid_t), &pData, &nData)) != 0) {
    goto _err;
  }

  tbDbKey.uid = suid;
  tbDbKey.version = ((SUidIdxVal *)pData)[0].version;
  if ((code = tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pData, &nData)) != 0) {
    goto _err;
  }

  tDecoderInit(&dc, pData, nData);
  code = metaDecodeEntry(&dc, &oStbEntry);
  if (code != 0) {
    goto _err;
  }

  SSchema *pCol = NULL;
  int32_t  colId = -1;
  for (int i = 0; i < oStbEntry.stbEntry.schemaTag.nCols; i++) {
    SSchema *schema = oStbEntry.stbEntry.schemaTag.pSchema + i;
    if (0 == strncmp(schema->name, pReq->colName, sizeof(pReq->colName))) {
      if (IS_IDX_ON(schema)) {
        pCol = schema;
      }
      break;
    }
  }

  if (pCol == NULL) {
    metaError("vgId:%d, failed to drop index on %s.%s,since %s", TD_VID(pMeta->pVnode), pReq->stb, pReq->colName,
              tstrerror(TSDB_CODE_VND_COL_NOT_EXISTS));
    code = 0;

    goto _err;
  }

  /*
   * iterator all pTdDbc by uid and version
   */
  TBC *pCtbIdxc = NULL;
  code = tdbTbcOpen(pMeta->pCtbIdx, &pCtbIdxc, NULL);
  if (code != 0) {
    goto _err;
  }

  code = tdbTbcMoveTo(pCtbIdxc, &(SCtbIdxKey){.suid = suid, .uid = INT64_MIN}, sizeof(SCtbIdxKey), &c);
  if (code < 0) {
    tdbTbcClose(pCtbIdxc);
    goto _err;
  }
  for (;;) {
    void *pKey = NULL, *pVal = NULL;
    int   nKey = 0, nVal = 0;

    code = tdbTbcNext(pCtbIdxc, &pKey, &nKey, &pVal, &nVal);
    if (code < 0) {
      tdbFree(pKey);
      tdbFree(pVal);
      tdbTbcClose(pCtbIdxc);
      pCtbIdxc = NULL;
      break;
    }
    if (((SCtbIdxKey *)pKey)->suid != suid) {
      tdbFree(pKey);
      tdbFree(pVal);
      continue;
    }
    STagIdxKey *pTagIdxKey = NULL;
    int32_t     nTagIdxKey;

    const void *pTagData = NULL;
    int32_t     nTagData = 0;

    SCtbIdxKey *table = (SCtbIdxKey *)pKey;
    STagVal     tagVal = {.cid = pCol->colId};
    if (tTagGet((const STag *)pVal, &tagVal)) {
      if (IS_VAR_DATA_TYPE(pCol->type)) {
        pTagData = tagVal.pData;
        nTagData = (int32_t)tagVal.nData;
      } else {
        pTagData = &(tagVal.i64);
        nTagData = tDataTypes[pCol->type].bytes;
      }
    } else {
      if (!IS_VAR_DATA_TYPE(pCol->type)) {
        nTagData = tDataTypes[pCol->type].bytes;
      }
    }

    code = metaCreateTagIdxKey(suid, pCol->colId, pTagData, nTagData, pCol->type, table->uid, &pTagIdxKey, &nTagIdxKey);
    tdbFree(pKey);
    tdbFree(pVal);
    if (code < 0) {
      metaDestroyTagIdxKey(pTagIdxKey);
      tdbTbcClose(pCtbIdxc);
      goto _err;
    }

    metaWLock(pMeta);
    ret = tdbTbDelete(pMeta->pTagIdx, pTagIdxKey, nTagIdxKey, pMeta->txn);
    if (ret < 0) {
      metaError("vgId:%d, failed to delete tag idx key:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->stb,
                pReq->stbUid, tstrerror(ret));
    }
    metaULock(pMeta);
    metaDestroyTagIdxKey(pTagIdxKey);
    pTagIdxKey = NULL;
  }

  // clear idx flag
  SSCHMEA_SET_IDX_OFF(pCol);

  nStbEntry.version = version;
  nStbEntry.type = TSDB_SUPER_TABLE;
  nStbEntry.uid = oStbEntry.uid;
  nStbEntry.name = oStbEntry.name;

  SSchemaWrapper  *row = tCloneSSchemaWrapper(&oStbEntry.stbEntry.schemaRow);
  SSchemaWrapper  *tag = tCloneSSchemaWrapper(&oStbEntry.stbEntry.schemaTag);
  SColCmprWrapper *cmpr = tCloneSColCmprWrapper(&oStbEntry.colCmpr);
  if (row == NULL || tag == NULL || cmpr == NULL) {
    tDeleteSchemaWrapper(row);
    tDeleteSchemaWrapper(tag);
    tDeleteSColCmprWrapper(cmpr);
    code = TSDB_CODE_OUT_OF_MEMORY;

    tdbTbcClose(pCtbIdxc);
    goto _err;
  }

  nStbEntry.stbEntry.schemaRow = *row;
  nStbEntry.stbEntry.schemaTag = *tag;
  nStbEntry.stbEntry.rsmaParam = oStbEntry.stbEntry.rsmaParam;
  nStbEntry.colCmpr = *cmpr;

  nStbEntry.colCmpr = oStbEntry.colCmpr;

  metaWLock(pMeta);
  // update table.db
  ret = metaSaveToTbDb(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to save tb db:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->stb,
              pReq->stbUid, tstrerror(ret));
  }
  // update uid index
  ret = metaUpdateUidIdx(pMeta, &nStbEntry);
  if (ret < 0) {
    metaError("vgId:%d, failed to update uid idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->stb,
              pReq->stbUid, tstrerror(ret));
  }
  metaULock(pMeta);

  tDeleteSchemaWrapper(tag);
  tDeleteSchemaWrapper(row);
  tDeleteSColCmprWrapper(cmpr);

  if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
  tDecoderClear(&dc);
  tdbFree(pData);

  tdbTbcClose(pCtbIdxc);
  return TSDB_CODE_SUCCESS;
_err:
  if (oStbEntry.pBuf) taosMemoryFree(oStbEntry.pBuf);
  tDecoderClear(&dc);
  tdbFree(pData);

  return code;
}

int metaCreateTable(SMeta *pMeta, int64_t ver, SVCreateTbReq *pReq, STableMetaRsp **pMetaRsp) {
  SMetaEntry  me = {0};
  SMetaReader mr = {0};
  int32_t     ret;

  // validate message
  if (pReq->type != TSDB_CHILD_TABLE && pReq->type != TSDB_NORMAL_TABLE) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _err;
  }

  if (pReq->type == TSDB_CHILD_TABLE) {
    tb_uid_t suid = metaGetTableEntryUidByName(pMeta, pReq->ctb.stbName);
    if (suid != pReq->ctb.suid) {
      return terrno = TSDB_CODE_PAR_TABLE_NOT_EXIST;
    }
  }

  // validate req
  metaReaderDoInit(&mr, pMeta, META_READER_LOCK);
  if (metaGetTableEntryByName(&mr, pReq->name) == 0) {
    if (pReq->type == TSDB_CHILD_TABLE && pReq->ctb.suid != mr.me.ctbEntry.suid) {
      metaReaderClear(&mr);
      return terrno = TSDB_CODE_TDB_TABLE_IN_OTHER_STABLE;
    }
    pReq->uid = mr.me.uid;
    if (pReq->type == TSDB_CHILD_TABLE) {
      pReq->ctb.suid = mr.me.ctbEntry.suid;
    }
    metaReaderClear(&mr);
    return terrno = TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
  } else if (terrno == TSDB_CODE_PAR_TABLE_NOT_EXIST) {
    terrno = TSDB_CODE_SUCCESS;
  }
  metaReaderClear(&mr);

  bool sysTbl = (pReq->type == TSDB_CHILD_TABLE) && metaTbInFilterCache(pMeta, pReq->ctb.stbName, 1);

  if (!sysTbl && ((terrno = grantCheck(TSDB_GRANT_TIMESERIES)) < 0)) goto _err;

  // build SMetaEntry
  SVnodeStats *pStats = &pMeta->pVnode->config.vndStats;
  me.version = ver;
  me.type = pReq->type;
  me.uid = pReq->uid;
  me.name = pReq->name;
  if (me.type == TSDB_CHILD_TABLE) {
    me.ctbEntry.btime = pReq->btime;
    me.ctbEntry.ttlDays = pReq->ttl;
    me.ctbEntry.commentLen = pReq->commentLen;
    me.ctbEntry.comment = pReq->comment;
    me.ctbEntry.suid = pReq->ctb.suid;
    me.ctbEntry.pTags = pReq->ctb.pTag;

#ifdef TAG_FILTER_DEBUG
    SArray *pTagVals = NULL;
    int32_t code = tTagToValArray((STag *)pReq->ctb.pTag, &pTagVals);
    for (int i = 0; i < taosArrayGetSize(pTagVals); i++) {
      STagVal *pTagVal = (STagVal *)taosArrayGet(pTagVals, i);

      if (IS_VAR_DATA_TYPE(pTagVal->type)) {
        char *buf = taosMemoryCalloc(pTagVal->nData + 1, 1);
        memcpy(buf, pTagVal->pData, pTagVal->nData);
        metaDebug("metaTag table:%s varchar index:%d cid:%d type:%d value:%s", pReq->name, i, pTagVal->cid,
                  pTagVal->type, buf);
        taosMemoryFree(buf);
      } else {
        double val = 0;
        GET_TYPED_DATA(val, double, pTagVal->type, &pTagVal->i64);
        metaDebug("metaTag table:%s number index:%d cid:%d type:%d value:%f", pReq->name, i, pTagVal->cid,
                  pTagVal->type, val);
      }
    }
#endif

    ++pStats->numOfCTables;

    if (!sysTbl) {
      int32_t nCols = 0;
      ret = metaGetStbStats(pMeta->pVnode, me.ctbEntry.suid, 0, &nCols);
      if (ret < 0) {
        metaError("vgId:%d, failed to get stb stats:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                  pReq->ctb.suid, tstrerror(ret));
      }
      pStats->numOfTimeSeries += nCols - 1;
    }

    metaWLock(pMeta);
    metaUpdateStbStats(pMeta, me.ctbEntry.suid, 1, 0);
    ret = metaUidCacheClear(pMeta, me.ctbEntry.suid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear uid cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->ctb.suid, tstrerror(ret));
    }
    ret = metaTbGroupCacheClear(pMeta, me.ctbEntry.suid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear group cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
                pReq->ctb.suid, tstrerror(ret));
    }
    metaULock(pMeta);

    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      ret = tsdbCacheNewTable(pMeta->pVnode->pTsdb, me.uid, me.ctbEntry.suid, NULL);
      if (ret < 0) {
        metaError("vgId:%d, failed to create table:%s since %s", TD_VID(pMeta->pVnode), pReq->name, tstrerror(ret));
        goto _err;
      }
    }
  } else {
    me.ntbEntry.btime = pReq->btime;
    me.ntbEntry.ttlDays = pReq->ttl;
    me.ntbEntry.commentLen = pReq->commentLen;
    me.ntbEntry.comment = pReq->comment;
    me.ntbEntry.schemaRow = pReq->ntb.schemaRow;
    me.ntbEntry.ncid = me.ntbEntry.schemaRow.pSchema[me.ntbEntry.schemaRow.nCols - 1].colId + 1;
    me.colCmpr = pReq->colCmpr;
    TABLE_SET_COL_COMPRESSED(me.flags);

    ++pStats->numOfNTables;
    pStats->numOfNTimeSeries += me.ntbEntry.schemaRow.nCols - 1;

    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      ret = tsdbCacheNewTable(pMeta->pVnode->pTsdb, me.uid, -1, &me.ntbEntry.schemaRow);
      if (ret < 0) {
        metaError("vgId:%d, failed to create table:%s since %s", TD_VID(pMeta->pVnode), pReq->name, tstrerror(ret));
        goto _err;
      }
    }
  }

  if (metaHandleEntry(pMeta, &me) < 0) goto _err;

  metaTimeSeriesNotifyCheck(pMeta);

  if (pMetaRsp) {
    *pMetaRsp = taosMemoryCalloc(1, sizeof(STableMetaRsp));

    if (*pMetaRsp) {
      if (me.type == TSDB_CHILD_TABLE) {
        (*pMetaRsp)->tableType = TSDB_CHILD_TABLE;
        (*pMetaRsp)->tuid = pReq->uid;
        (*pMetaRsp)->suid = pReq->ctb.suid;
        tstrncpy((*pMetaRsp)->tbName, pReq->name, strlen(pReq->name) + 1);
      } else {
        ret = metaUpdateMetaRsp(pReq->uid, pReq->name, &pReq->ntb.schemaRow, *pMetaRsp);
        if (ret < 0) {
          metaError("vgId:%d, failed to update meta rsp:%s since %s", TD_VID(pMeta->pVnode), pReq->name,
                    tstrerror(ret));
        }
        for (int32_t i = 0; i < pReq->colCmpr.nCols; i++) {
          SColCmpr *p = &pReq->colCmpr.pColCmpr[i];
          (*pMetaRsp)->pSchemaExt[i].colId = p->id;
          (*pMetaRsp)->pSchemaExt[i].compress = p->alg;
        }
      }
    }
  }

  pMeta->changed = true;
  metaDebug("vgId:%d, table:%s uid %" PRId64 " is created, type:%" PRId8, TD_VID(pMeta->pVnode), pReq->name, pReq->uid,
            pReq->type);
  return 0;

_err:
  metaError("vgId:%d, failed to create table:%s type:%s since %s", TD_VID(pMeta->pVnode), pReq->name,
            pReq->type == TSDB_CHILD_TABLE ? "child table" : "normal table", tstrerror(terrno));
  return TSDB_CODE_FAILED;
}

int metaDropTable(SMeta *pMeta, int64_t version, SVDropTbReq *pReq, SArray *tbUids, tb_uid_t *tbUid) {
  void    *pData = NULL;
  int      nData = 0;
  int      rc = 0;
  tb_uid_t uid = 0;
  tb_uid_t suid = 0;
  int8_t   sysTbl = 0;
  int      type;

  rc = tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &pData, &nData);
  if (rc < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  uid = *(tb_uid_t *)pData;

  metaWLock(pMeta);
  rc = metaDropTableByUid(pMeta, uid, &type, &suid, &sysTbl);
  metaULock(pMeta);

  if (rc < 0) goto _exit;

  if (!sysTbl && type == TSDB_CHILD_TABLE) {
    int32_t      nCols = 0;
    SVnodeStats *pStats = &pMeta->pVnode->config.vndStats;
    if (metaGetStbStats(pMeta->pVnode, suid, NULL, &nCols) == 0) {
      pStats->numOfTimeSeries -= nCols - 1;
    }
  }

  if ((type == TSDB_CHILD_TABLE || type == TSDB_NORMAL_TABLE) && tbUids) {
    if (taosArrayPush(tbUids, &uid) == NULL) {
      rc = terrno;
      goto _exit;
    }

    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      int32_t ret = tsdbCacheDropTable(pMeta->pVnode->pTsdb, uid, suid, NULL);
      if (ret < 0) {
        metaError("vgId:%d, failed to drop table:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, uid,
                  tstrerror(ret));
      }
    }
  }

  if ((type == TSDB_CHILD_TABLE) && tbUid) {
    *tbUid = uid;
  }

  pMeta->changed = true;
_exit:
  tdbFree(pData);
  return rc;
}

int32_t metaDropTables(SMeta *pMeta, SArray *tbUids) {
  int32_t code = 0;
  if (taosArrayGetSize(tbUids) == 0) return TSDB_CODE_SUCCESS;

  int64_t    nCtbDropped = 0;
  SSHashObj *suidHash = tSimpleHashInit(16, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT));
  if (suidHash == NULL) {
    return terrno;
  }

  metaWLock(pMeta);
  for (int i = 0; i < taosArrayGetSize(tbUids); ++i) {
    tb_uid_t uid = *(tb_uid_t *)taosArrayGet(tbUids, i);
    tb_uid_t suid = 0;
    int8_t   sysTbl = 0;
    int      type;
    code = metaDropTableByUid(pMeta, uid, &type, &suid, &sysTbl);
    if (code) return code;
    if (!sysTbl && type == TSDB_CHILD_TABLE && suid != 0 && suidHash) {
      int64_t *pVal = tSimpleHashGet(suidHash, &suid, sizeof(tb_uid_t));
      if (pVal) {
        nCtbDropped = *pVal + 1;
      } else {
        nCtbDropped = 1;
      }
      code = tSimpleHashPut(suidHash, &suid, sizeof(tb_uid_t), &nCtbDropped, sizeof(int64_t));
      if (code) return code;
    }
    /*
    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      tsdbCacheDropTable(pMeta->pVnode->pTsdb, uid, suid, NULL);
    }
    */
    metaDebug("batch drop table:%" PRId64, uid);
  }
  metaULock(pMeta);

  // update timeseries
  void   *pCtbDropped = NULL;
  int32_t iter = 0;
  while ((pCtbDropped = tSimpleHashIterate(suidHash, pCtbDropped, &iter))) {
    tb_uid_t    *pSuid = tSimpleHashGetKey(pCtbDropped, NULL);
    int32_t      nCols = 0;
    SVnodeStats *pStats = &pMeta->pVnode->config.vndStats;
    if (metaGetStbStats(pMeta->pVnode, *pSuid, NULL, &nCols) == 0) {
      pStats->numOfTimeSeries -= *(int64_t *)pCtbDropped * (nCols - 1);
    }
  }
  tSimpleHashCleanup(suidHash);

  pMeta->changed = true;
  return 0;
}

static int32_t metaFilterTableByHash(SMeta *pMeta, SArray *uidList) {
  int32_t code = 0;
  // 1, tranverse table's
  // 2, validate table name using vnodeValidateTableHash
  // 3, push invalidated table's uid into uidList

  TBC *pCur;
  code = tdbTbcOpen(pMeta->pTbDb, &pCur, NULL);
  if (code < 0) {
    return code;
  }

  code = tdbTbcMoveToFirst(pCur);
  if (code) {
    tdbTbcClose(pCur);
    return code;
  }

  void *pData = NULL, *pKey = NULL;
  int   nData = 0, nKey = 0;

  while (1) {
    int32_t ret = tdbTbcNext(pCur, &pKey, &nKey, &pData, &nData);
    if (ret < 0) {
      break;
    }

    SMetaEntry me = {0};
    SDecoder   dc = {0};
    tDecoderInit(&dc, pData, nData);
    code = metaDecodeEntry(&dc, &me);
    if (code < 0) {
      tDecoderClear(&dc);
      return code;
    }

    if (me.type != TSDB_SUPER_TABLE) {
      char tbFName[TSDB_TABLE_FNAME_LEN + 1];
      snprintf(tbFName, sizeof(tbFName), "%s.%s", pMeta->pVnode->config.dbname, me.name);
      tbFName[TSDB_TABLE_FNAME_LEN] = '\0';
      int32_t ret = vnodeValidateTableHash(pMeta->pVnode, tbFName);
      if (ret < 0 && terrno == TSDB_CODE_VND_HASH_MISMATCH) {
        if (taosArrayPush(uidList, &me.uid) == NULL) {
          code = terrno;
          break;
        }
      }
    }
    tDecoderClear(&dc);
  }
  tdbFree(pData);
  tdbFree(pKey);
  tdbTbcClose(pCur);

  return 0;
}

int32_t metaTrimTables(SMeta *pMeta) {
  int32_t code = 0;

  SArray *tbUids = taosArrayInit(8, sizeof(int64_t));
  if (tbUids == NULL) {
    return terrno;
  }

  code = metaFilterTableByHash(pMeta, tbUids);
  if (code != 0) {
    goto end;
  }
  if (TARRAY_SIZE(tbUids) == 0) {
    goto end;
  }

  metaInfo("vgId:%d, trim %ld tables", TD_VID(pMeta->pVnode), taosArrayGetSize(tbUids));
  code = metaDropTables(pMeta, tbUids);
  if (code) goto end;

end:
  taosArrayDestroy(tbUids);

  return code;
}

int metaTtlFindExpired(SMeta *pMeta, int64_t timePointMs, SArray *tbUids, int32_t ttlDropMaxCount) {
  metaRLock(pMeta);

  int ret = ttlMgrFindExpired(pMeta->pTtlMgr, timePointMs, tbUids, ttlDropMaxCount);

  metaULock(pMeta);

  if (ret != 0) {
    metaError("ttl failed to find expired table, ret:%d", ret);
  }

  return ret;
}

static int metaBuildBtimeIdxKey(SBtimeIdxKey *btimeKey, const SMetaEntry *pME) {
  int64_t btime;
  if (pME->type == TSDB_CHILD_TABLE) {
    btime = pME->ctbEntry.btime;
  } else if (pME->type == TSDB_NORMAL_TABLE) {
    btime = pME->ntbEntry.btime;
  } else {
    return TSDB_CODE_FAILED;
  }

  btimeKey->btime = btime;
  btimeKey->uid = pME->uid;
  return 0;
}

static int metaBuildNColIdxKey(SNcolIdxKey *ncolKey, const SMetaEntry *pME) {
  if (pME->type == TSDB_NORMAL_TABLE) {
    ncolKey->ncol = pME->ntbEntry.schemaRow.nCols;
    ncolKey->uid = pME->uid;
  } else {
    return TSDB_CODE_FAILED;
  }
  return 0;
}

static void metaDeleteTtl(SMeta *pMeta, const SMetaEntry *pME) {
  if (pME->type != TSDB_CHILD_TABLE && pME->type != TSDB_NORMAL_TABLE) return;

  STtlDelTtlCtx ctx = {.uid = pME->uid, .pTxn = pMeta->txn};
  if (pME->type == TSDB_CHILD_TABLE) {
    ctx.ttlDays = pME->ctbEntry.ttlDays;
  } else {
    ctx.ttlDays = pME->ntbEntry.ttlDays;
  }

  int32_t ret = ttlMgrDeleteTtl(pMeta->pTtlMgr, &ctx);
  if (ret < 0) {
    metaError("vgId:%d, failed to delete ttl for table:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pME->name,
              pME->uid, tstrerror(ret));
  }
  return;
}

static int metaDropTableByUid(SMeta *pMeta, tb_uid_t uid, int *type, tb_uid_t *pSuid, int8_t *pSysTbl) {
  void      *pData = NULL;
  int        nData = 0;
  int        rc = 0;
  SMetaEntry e = {0};
  SDecoder   dc = {0};
  int32_t    ret = 0;

  rc = tdbTbGet(pMeta->pUidIdx, &uid, sizeof(uid), &pData, &nData);
  if (rc < 0) {
    return rc;
  }
  int64_t version = ((SUidIdxVal *)pData)[0].version;

  rc = tdbTbGet(pMeta->pTbDb, &(STbDbKey){.version = version, .uid = uid}, sizeof(STbDbKey), &pData, &nData);
  if (rc < 0) {
    tdbFree(pData);
    return rc;
  }

  tDecoderInit(&dc, pData, nData);
  rc = metaDecodeEntry(&dc, &e);
  if (rc < 0) {
    tDecoderClear(&dc);
    return rc;
  }

  if (type) *type = e.type;

  if (e.type == TSDB_CHILD_TABLE) {
    if (pSuid) *pSuid = e.ctbEntry.suid;
    void *tData = NULL;
    int   tLen = 0;

    if (tdbTbGet(pMeta->pUidIdx, &e.ctbEntry.suid, sizeof(tb_uid_t), &tData, &tLen) == 0) {
      STbDbKey tbDbKey = {.uid = e.ctbEntry.suid, .version = ((SUidIdxVal *)tData)[0].version};
      if (tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &tData, &tLen) == 0) {
        SDecoder   tdc = {0};
        SMetaEntry stbEntry = {0};

        tDecoderInit(&tdc, tData, tLen);
        int32_t ret = metaDecodeEntry(&tdc, &stbEntry);
        if (ret < 0) {
          tDecoderClear(&tdc);
          metaError("vgId:%d, failed to decode child table:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name,
                    e.ctbEntry.suid, tstrerror(ret));
          return ret;
        }

        if (pSysTbl) *pSysTbl = metaTbInFilterCache(pMeta, stbEntry.name, 1) ? 1 : 0;

        SSchema        *pTagColumn = NULL;
        SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
        if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
          pTagColumn = &stbEntry.stbEntry.schemaTag.pSchema[0];
          ret = metaDelJsonVarFromIdx(pMeta, &e, pTagColumn);
          if (ret < 0) {
            metaError("vgId:%d, failed to delete json var from idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode),
                      e.name, e.uid, tstrerror(ret));
          }
        } else {
          for (int i = 0; i < pTagSchema->nCols; i++) {
            pTagColumn = &stbEntry.stbEntry.schemaTag.pSchema[i];
            if (!IS_IDX_ON(pTagColumn)) continue;
            STagIdxKey *pTagIdxKey = NULL;
            int32_t     nTagIdxKey;

            const void *pTagData = NULL;
            int32_t     nTagData = 0;

            STagVal tagVal = {.cid = pTagColumn->colId};
            if (tTagGet((const STag *)e.ctbEntry.pTags, &tagVal)) {
              if (IS_VAR_DATA_TYPE(pTagColumn->type)) {
                pTagData = tagVal.pData;
                nTagData = (int32_t)tagVal.nData;
              } else {
                pTagData = &(tagVal.i64);
                nTagData = tDataTypes[pTagColumn->type].bytes;
              }
            } else {
              if (!IS_VAR_DATA_TYPE(pTagColumn->type)) {
                nTagData = tDataTypes[pTagColumn->type].bytes;
              }
            }

            if (metaCreateTagIdxKey(e.ctbEntry.suid, pTagColumn->colId, pTagData, nTagData, pTagColumn->type, uid,
                                    &pTagIdxKey, &nTagIdxKey) == 0) {
              ret = tdbTbDelete(pMeta->pTagIdx, pTagIdxKey, nTagIdxKey, pMeta->txn);
              if (ret < 0) {
                metaError("vgId:%d, failed to delete tag idx key:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode),
                          e.name, e.uid, tstrerror(ret));
              }
            }
            metaDestroyTagIdxKey(pTagIdxKey);
            pTagIdxKey = NULL;
          }
        }
        tDecoderClear(&tdc);
      }
      tdbFree(tData);
    }
  }

  ret = tdbTbDelete(pMeta->pTbDb, &(STbDbKey){.version = version, .uid = uid}, sizeof(STbDbKey), pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to delete table:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
              tstrerror(ret));
  }
  ret = tdbTbDelete(pMeta->pNameIdx, e.name, strlen(e.name) + 1, pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to delete name idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
              tstrerror(ret));
  }
  ret = tdbTbDelete(pMeta->pUidIdx, &uid, sizeof(uid), pMeta->txn);
  if (ret < 0) {
    metaError("vgId:%d, failed to delete uid idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
              tstrerror(ret));
  }

  if (e.type == TSDB_CHILD_TABLE || e.type == TSDB_NORMAL_TABLE) metaDeleteBtimeIdx(pMeta, &e);
  if (e.type == TSDB_NORMAL_TABLE) metaDeleteNcolIdx(pMeta, &e);

  if (e.type != TSDB_SUPER_TABLE) metaDeleteTtl(pMeta, &e);

  if (e.type == TSDB_CHILD_TABLE) {
    ret =
        tdbTbDelete(pMeta->pCtbIdx, &(SCtbIdxKey){.suid = e.ctbEntry.suid, .uid = uid}, sizeof(SCtbIdxKey), pMeta->txn);
    if (ret < 0) {
      metaError("vgId:%d, failed to delete ctb idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
                tstrerror(ret));
    }

    --pMeta->pVnode->config.vndStats.numOfCTables;
    metaUpdateStbStats(pMeta, e.ctbEntry.suid, -1, 0);
    ret = metaUidCacheClear(pMeta, e.ctbEntry.suid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear uid cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name,
                e.ctbEntry.suid, tstrerror(ret));
    }
    ret = metaTbGroupCacheClear(pMeta, e.ctbEntry.suid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear group cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name,
                e.ctbEntry.suid, tstrerror(ret));
    }
    /*
    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      tsdbCacheDropTable(pMeta->pVnode->pTsdb, e.uid, e.ctbEntry.suid, NULL);
    }
    */
  } else if (e.type == TSDB_NORMAL_TABLE) {
    // drop schema.db (todo)

    --pMeta->pVnode->config.vndStats.numOfNTables;
    pMeta->pVnode->config.vndStats.numOfNTimeSeries -= e.ntbEntry.schemaRow.nCols - 1;

    /*
    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      tsdbCacheDropTable(pMeta->pVnode->pTsdb, e.uid, -1, &e.ntbEntry.schemaRow);
    }
    */
  } else if (e.type == TSDB_SUPER_TABLE) {
    ret = tdbTbDelete(pMeta->pSuidIdx, &e.uid, sizeof(tb_uid_t), pMeta->txn);
    if (ret < 0) {
      metaError("vgId:%d, failed to delete suid idx:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
                tstrerror(ret));
    }
    // drop schema.db (todo)

    ret = metaStatsCacheDrop(pMeta, uid);
    if (ret < 0) {
      metaError("vgId:%d, failed to drop stats cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
                tstrerror(ret));
    }
    ret = metaUidCacheClear(pMeta, uid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear uid cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
                tstrerror(ret));
    }
    ret = metaTbGroupCacheClear(pMeta, uid);
    if (ret < 0) {
      metaError("vgId:%d, failed to clear group cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name,
                e.uid, tstrerror(ret));
    }
    --pMeta->pVnode->config.vndStats.numOfSTables;
  }

  ret = metaCacheDrop(pMeta, uid);
  if (ret < 0) {
    metaError("vgId:%d, failed to drop cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), e.name, e.uid,
              tstrerror(ret));
  }

  tDecoderClear(&dc);
  tdbFree(pData);

  return 0;
}
// opt ins_tables
int metaUpdateBtimeIdx(SMeta *pMeta, const SMetaEntry *pME) {
  SBtimeIdxKey btimeKey = {0};
  if (metaBuildBtimeIdxKey(&btimeKey, pME) < 0) {
    return 0;
  }
  metaTrace("vgId:%d, start to save version:%" PRId64 " uid:%" PRId64 " btime:%" PRId64, TD_VID(pMeta->pVnode),
            pME->version, pME->uid, btimeKey.btime);

  return tdbTbUpsert(pMeta->pBtimeIdx, &btimeKey, sizeof(btimeKey), NULL, 0, pMeta->txn);
}

int metaDeleteBtimeIdx(SMeta *pMeta, const SMetaEntry *pME) {
  SBtimeIdxKey btimeKey = {0};
  if (metaBuildBtimeIdxKey(&btimeKey, pME) < 0) {
    return 0;
  }
  return tdbTbDelete(pMeta->pBtimeIdx, &btimeKey, sizeof(btimeKey), pMeta->txn);
}
int metaUpdateNcolIdx(SMeta *pMeta, const SMetaEntry *pME) {
  SNcolIdxKey ncolKey = {0};
  if (metaBuildNColIdxKey(&ncolKey, pME) < 0) {
    return 0;
  }
  return tdbTbUpsert(pMeta->pNcolIdx, &ncolKey, sizeof(ncolKey), NULL, 0, pMeta->txn);
}

int metaDeleteNcolIdx(SMeta *pMeta, const SMetaEntry *pME) {
  SNcolIdxKey ncolKey = {0};
  if (metaBuildNColIdxKey(&ncolKey, pME) < 0) {
    return 0;
  }
  return tdbTbDelete(pMeta->pNcolIdx, &ncolKey, sizeof(ncolKey), pMeta->txn);
}

static int metaAlterTableColumn(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq, STableMetaRsp *pMetaRsp) {
  void           *pVal = NULL;
  int             nVal = 0;
  const void     *pData = NULL;
  int             nData = 0;
  int             ret = 0;
  tb_uid_t        uid;
  int64_t         oversion;
  SSchema        *pColumn = NULL;
  SMetaEntry      entry = {0};
  SSchemaWrapper *pSchema;
  int             c;
  bool            freeColCmpr = false;
  if (pAlterTbReq->colName == NULL) {
    metaError("meta/table: null pAlterTbReq->colName");
    return terrno = TSDB_CODE_INVALID_MSG;
  }

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  uid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  // search uid index
  TBC *pUidIdxc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pUidIdx, &pUidIdxc, NULL));
  ret = tdbTbcMoveTo(pUidIdxc, &uid, sizeof(uid), &c);
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: invalide c: %" PRId32 " alt tb column failed.", c);
    return TSDB_CODE_FAILED;
  }

  ret = tdbTbcGet(pUidIdxc, NULL, NULL, &pData, &nData);
  oversion = ((SUidIdxVal *)pData)[0].version;

  // search table.db
  TBC *pTbDbc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTbDb, &pTbDbc, NULL));
  ret = tdbTbcMoveTo(pTbDbc, &((STbDbKey){.uid = uid, .version = oversion}), sizeof(STbDbKey), &c);
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    metaError("meta/table: invalide c: %" PRId32 " alt tb column failed.", c);
    return TSDB_CODE_FAILED;
  }

  ret = tdbTbcGet(pTbDbc, NULL, NULL, &pData, &nData);

  // get table entry
  SDecoder dc = {0};
  if ((entry.pBuf = taosMemoryMalloc(nData)) == NULL) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno;
  }
  memcpy(entry.pBuf, pData, nData);
  tDecoderInit(&dc, entry.pBuf, nData);
  ret = metaDecodeEntry(&dc, &entry);
  if (ret != 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    tDecoderClear(&dc);
    metaError("meta/table: invalide ret: %" PRId32 " alt tb column failed.", ret);
    return ret;
  }

  if (entry.type != TSDB_NORMAL_TABLE) {
    terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
    goto _err;
  }
  // search the column to add/drop/update
  pSchema = &entry.ntbEntry.schemaRow;

  // save old entry
  SMetaEntry oldEntry = {.type = TSDB_NORMAL_TABLE, .uid = entry.uid};
  oldEntry.ntbEntry.schemaRow.nCols = pSchema->nCols;

  int32_t rowLen = -1;
  if (pAlterTbReq->action == TSDB_ALTER_TABLE_ADD_COLUMN ||
      pAlterTbReq->action == TSDB_ALTER_TABLE_UPDATE_COLUMN_BYTES) {
    rowLen = 0;
  }

  int32_t  iCol = 0, jCol = 0;
  SSchema *qColumn = NULL;
  for (;;) {
    qColumn = NULL;

    if (jCol >= pSchema->nCols) break;
    qColumn = &pSchema->pSchema[jCol];

    if (!pColumn && (strcmp(qColumn->name, pAlterTbReq->colName) == 0)) {
      pColumn = qColumn;
      iCol = jCol;
      if (rowLen < 0) break;
    }
    rowLen += qColumn->bytes;
    ++jCol;
  }

  entry.version = version;
  int      tlen;
  SSchema *pNewSchema = NULL;
  SSchema  tScheam;
  switch (pAlterTbReq->action) {
    case TSDB_ALTER_TABLE_ADD_COLUMN:
    case TSDB_ALTER_TABLE_ADD_COLUMN_WITH_COMPRESS_OPTION:
      if (pColumn) {
        terrno = TSDB_CODE_VND_COL_ALREADY_EXISTS;
        goto _err;
      }
      if ((terrno = grantCheck(TSDB_GRANT_TIMESERIES)) < 0) {
        goto _err;
      }
      if (rowLen + pAlterTbReq->bytes > TSDB_MAX_BYTES_PER_ROW) {
        terrno = TSDB_CODE_PAR_INVALID_ROW_LENGTH;
        goto _err;
      }
      pSchema->version++;
      pSchema->nCols++;
      pNewSchema = taosMemoryMalloc(sizeof(SSchema) * pSchema->nCols);
      if (pNewSchema == NULL) {
        goto _err;
      }
      memcpy(pNewSchema, pSchema->pSchema, sizeof(SSchema) * (pSchema->nCols - 1));
      pSchema->pSchema = pNewSchema;
      pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].bytes = pAlterTbReq->bytes;
      pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].type = pAlterTbReq->type;
      pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].flags = pAlterTbReq->flags;
      pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].colId = entry.ntbEntry.ncid++;
      tstrncpy(pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].name, pAlterTbReq->colName,
               strlen(pAlterTbReq->colName) + 1);

      ++pMeta->pVnode->config.vndStats.numOfNTimeSeries;
      metaTimeSeriesNotifyCheck(pMeta);

      if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
        int16_t cid = pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].colId;
        int8_t  col_type = pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1].type;
        int32_t ret = tsdbCacheNewNTableColumn(pMeta->pVnode->pTsdb, entry.uid, cid, col_type);
        if (ret < 0) {
          terrno = ret;
          goto _err;
        }
      }
      SSchema *pCol = &pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1];
      uint32_t compress = pAlterTbReq->action == TSDB_ALTER_TABLE_ADD_COLUMN ? createDefaultColCmprByType(pCol->type)
                                                                             : pAlterTbReq->compress;
      if (updataTableColCmpr(&entry.colCmpr, pCol, 1, compress) != 0) {
        metaError("vgId:%d, failed to update table col cmpr:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name,
                  entry.uid);
      }
      freeColCmpr = true;
      if (entry.colCmpr.nCols != pSchema->nCols) {
        if (pNewSchema) taosMemoryFree(pNewSchema);
        if (freeColCmpr) taosMemoryFree(entry.colCmpr.pColCmpr);
        terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
        goto _err;
      }
      break;
    case TSDB_ALTER_TABLE_DROP_COLUMN:
      if (pColumn == NULL) {
        terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
        goto _err;
      }
      if (pColumn->colId == 0) {
        terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
        goto _err;
      }
      if (tqCheckColModifiable(pMeta->pVnode->pTq, uid, pColumn->colId) != 0) {
        terrno = TSDB_CODE_VND_COL_SUBSCRIBED;
        goto _err;
      }
      bool hasPrimayKey = false;
      if (pSchema->nCols >= 2) {
        hasPrimayKey = pSchema->pSchema[1].flags & COL_IS_KEY ? true : false;
      }

      memcpy(&tScheam, pColumn, sizeof(SSchema));
      pSchema->version++;
      tlen = (pSchema->nCols - iCol - 1) * sizeof(SSchema);
      if (tlen) {
        memmove(pColumn, pColumn + 1, tlen);
      }
      pSchema->nCols--;

      --pMeta->pVnode->config.vndStats.numOfNTimeSeries;

      if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
        int16_t cid = pColumn->colId;

        if (tsdbCacheDropNTableColumn(pMeta->pVnode->pTsdb, entry.uid, cid, hasPrimayKey) != 0) {
          metaError("vgId:%d, failed to drop ntable column:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name,
                    entry.uid);
        }
      }

      if (updataTableColCmpr(&entry.colCmpr, &tScheam, 0, 0) != 0) {
        metaError("vgId:%d, failed to update table col cmpr:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name,
                  entry.uid);
      }
      if (entry.colCmpr.nCols != pSchema->nCols) {
        terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
        goto _err;
      }
      break;
    case TSDB_ALTER_TABLE_UPDATE_COLUMN_BYTES:
      if (pColumn == NULL) {
        terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
        goto _err;
      }
      if (!IS_VAR_DATA_TYPE(pColumn->type) || pColumn->bytes >= pAlterTbReq->colModBytes) {
        terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
        goto _err;
      }
      if (rowLen + pAlterTbReq->colModBytes - pColumn->bytes > TSDB_MAX_BYTES_PER_ROW) {
        terrno = TSDB_CODE_PAR_INVALID_ROW_LENGTH;
        goto _err;
      }
      if (tqCheckColModifiable(pMeta->pVnode->pTq, uid, pColumn->colId) != 0) {
        terrno = TSDB_CODE_VND_COL_SUBSCRIBED;
        goto _err;
      }
      pSchema->version++;
      pColumn->bytes = pAlterTbReq->colModBytes;
      break;
    case TSDB_ALTER_TABLE_UPDATE_COLUMN_NAME:
      if (pAlterTbReq->colNewName == NULL) {
        terrno = TSDB_CODE_INVALID_MSG;
        goto _err;
      }
      if (pColumn == NULL) {
        terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
        goto _err;
      }
      if (tqCheckColModifiable(pMeta->pVnode->pTq, uid, pColumn->colId) != 0) {
        terrno = TSDB_CODE_VND_COL_SUBSCRIBED;
        goto _err;
      }
      pSchema->version++;
      tstrncpy(pColumn->name, pAlterTbReq->colNewName, strlen(pAlterTbReq->colNewName) + 1);
      break;
  }

  if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
    tsdbCacheInvalidateSchema(pMeta->pVnode->pTsdb, 0, entry.uid, pSchema->version);
  }

  entry.version = version;

  // do actual write
  metaWLock(pMeta);

  if (metaDeleteNcolIdx(pMeta, &oldEntry) < 0) {
    metaError("vgId:%d, failed to delete ncol idx:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  if (metaUpdateNcolIdx(pMeta, &entry) < 0) {
    metaError("vgId:%d, failed to update ncol idx:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  // save to table db
  if (metaSaveToTbDb(pMeta, &entry) < 0) {
    metaError("vgId:%d, failed to save to tb db:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  if (metaUpdateUidIdx(pMeta, &entry) < 0) {
    metaError("vgId:%d, failed to update uid idx:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  if (metaSaveToSkmDb(pMeta, &entry) < 0) {
    metaError("vgId:%d, failed to save to skm db:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  if (metaUpdateChangeTime(pMeta, entry.uid, pAlterTbReq->ctimeMs) < 0) {
    metaError("vgId:%d, failed to update change time:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }

  metaULock(pMeta);

  if (metaUpdateMetaRsp(uid, pAlterTbReq->tbName, pSchema, pMetaRsp) < 0) {
    metaError("vgId:%d, failed to update meta rsp:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }
  for (int32_t i = 0; i < entry.colCmpr.nCols; i++) {
    SColCmpr *p = &entry.colCmpr.pColCmpr[i];
    pMetaRsp->pSchemaExt[i].colId = p->id;
    pMetaRsp->pSchemaExt[i].compress = p->alg;
  }

  if (entry.pBuf) taosMemoryFree(entry.pBuf);
  if (pNewSchema) taosMemoryFree(pNewSchema);
  if (freeColCmpr) taosMemoryFree(entry.colCmpr.pColCmpr);

  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  tDecoderClear(&dc);

  return 0;

_err:
  if (entry.pBuf) taosMemoryFree(entry.pBuf);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  tDecoderClear(&dc);

  return terrno != 0 ? terrno : TSDB_CODE_FAILED;
}

static int metaUpdateTableMultiTagVal(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq) {
  SMetaEntry  ctbEntry = {0};
  SMetaEntry  stbEntry = {0};
  void       *pVal = NULL;
  int         nVal = 0;
  int         ret;
  int         c;
  tb_uid_t    uid;
  int64_t     oversion;
  const void *pData = NULL;
  int         nData = 0;
  SHashObj   *pTagTable = NULL;

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  uid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  // search uid index
  TBC *pUidIdxc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pUidIdx, &pUidIdxc, NULL));
  if (tdbTbcMoveTo(pUidIdxc, &uid, sizeof(uid), &c) < 0) {
    metaTrace("meta/table: failed to move to uid index, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: invalide c: %" PRId32 " update tb tag val failed.", c);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  if (tdbTbcGet(pUidIdxc, NULL, NULL, &pData, &nData) != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: failed to get uid index, uid:%" PRId64, uid);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  oversion = ((SUidIdxVal *)pData)[0].version;

  // search table.db
  TBC     *pTbDbc = NULL;
  SDecoder dc1 = {0};
  SDecoder dc2 = {0};

  /* get ctbEntry */
  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTbDb, &pTbDbc, NULL));
  if (tdbTbcMoveTo(pTbDbc, &((STbDbKey){.uid = uid, .version = oversion}), sizeof(STbDbKey), &c) != 0) {
    metaError("meta/table: failed to move to tb db, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    metaError("meta/table: invalide c: %" PRId32 " update tb tag val failed.", c);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  if (tdbTbcGet(pTbDbc, NULL, NULL, &pData, &nData) != 0) {
    metaError("meta/table: failed to get tb db, uid:%" PRId64, uid);
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  if ((ctbEntry.pBuf = taosMemoryMalloc(nData)) == NULL) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno;
  }
  memcpy(ctbEntry.pBuf, pData, nData);
  tDecoderInit(&dc1, ctbEntry.pBuf, nData);
  ret = metaDecodeEntry(&dc1, &ctbEntry);
  if (ret < 0) {
    terrno = ret;
    goto _err;
  }

  /* get stbEntry*/
  if (tdbTbGet(pMeta->pUidIdx, &ctbEntry.ctbEntry.suid, sizeof(tb_uid_t), &pVal, &nVal) != 0) {
    metaError("meta/table: failed to get uid index, uid:%" PRId64, ctbEntry.ctbEntry.suid);
  }
  if (!pVal) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _err;
  }

  if (tdbTbGet(pMeta->pTbDb, &((STbDbKey){.uid = ctbEntry.ctbEntry.suid, .version = ((SUidIdxVal *)pVal)[0].version}),
               sizeof(STbDbKey), (void **)&stbEntry.pBuf, &nVal) != 0) {
    metaError("meta/table: failed to get tb db, uid:%" PRId64, ctbEntry.ctbEntry.suid);
  }
  tdbFree(pVal);
  tDecoderInit(&dc2, stbEntry.pBuf, nVal);
  ret = metaDecodeEntry(&dc2, &stbEntry);
  if (ret < 0) {
    terrno = ret;
    goto _err;
  }

  int32_t nTagVals = taosArrayGetSize(pAlterTbReq->pMultiTag);
  pTagTable = taosHashInit(nTagVals, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_NO_LOCK);
  if (pTagTable == NULL) {
    ret = terrno;
    goto _err;
  }

  // remove duplicate tag name
  for (int i = 0; i < nTagVals; i++) {
    SMultiTagUpateVal *pTagVal = taosArrayGet(pAlterTbReq->pMultiTag, i);
    ret = taosHashPut(pTagTable, pTagVal->tagName, strlen(pTagVal->tagName), pTagVal, sizeof(*pTagVal));
    if (ret != 0) {
      goto _err;
    }
  }

  SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
  SSchema        *pColumn = NULL;
  int32_t         iCol = 0;
  int32_t         count = 0;

  for (;;) {
    pColumn = NULL;

    if (iCol >= pTagSchema->nCols) break;
    pColumn = &pTagSchema->pSchema[iCol];
    if (taosHashGet(pTagTable, pColumn->name, strlen(pColumn->name)) != NULL) {
      count++;
    }
    iCol++;
  }
  if (count != taosHashGetSize(pTagTable)) {
    terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
    goto _err;
  }

  ctbEntry.version = version;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
    goto _err;
  } else {
    const STag *pOldTag = (const STag *)ctbEntry.ctbEntry.pTags;
    STag       *pNewTag = NULL;
    SArray     *pTagArray = taosArrayInit(pTagSchema->nCols, sizeof(STagVal));
    if (!pTagArray) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    for (int32_t i = 0; i < pTagSchema->nCols; i++) {
      SSchema           *pCol = &pTagSchema->pSchema[i];
      SMultiTagUpateVal *pTagVal = taosHashGet(pTagTable, pCol->name, strlen(pCol->name));
      if (pTagVal == NULL) {
        STagVal val = {.cid = pCol->colId};
        if (tTagGet(pOldTag, &val)) {
          if (taosArrayPush(pTagArray, &val) == NULL) {
            terrno = TSDB_CODE_OUT_OF_MEMORY;
            taosArrayDestroy(pTagArray);
            goto _err;
          }
        }
      } else {
        STagVal val = {0};
        val.type = pCol->type;
        val.cid = pCol->colId;
        if (pTagVal->isNull) continue;

        if (IS_VAR_DATA_TYPE(pCol->type)) {
          val.pData = pTagVal->pTagVal;
          val.nData = pTagVal->nTagVal;
        } else {
          memcpy(&val.i64, pTagVal->pTagVal, pTagVal->nTagVal);
        }
        if (taosArrayPush(pTagArray, &val) == NULL) {
          terrno = TSDB_CODE_OUT_OF_MEMORY;
          taosArrayDestroy(pTagArray);
          goto _err;
        }
      }
    }
    if ((terrno = tTagNew(pTagArray, pTagSchema->version, false, &pNewTag)) < 0) {
      taosArrayDestroy(pTagArray);
      goto _err;
    }
    ctbEntry.ctbEntry.pTags = (uint8_t *)pNewTag;
    taosArrayDestroy(pTagArray);
  }

  metaWLock(pMeta);

  // save to table.db
  if (metaSaveToTbDb(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to save to tb db:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  // save to uid.idx
  if (metaUpdateUidIdx(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to update uid idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUpdateTagIdx(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to update tag idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  SCtbIdxKey ctbIdxKey = {.suid = ctbEntry.ctbEntry.suid, .uid = uid};
  if (tdbTbUpsert(pMeta->pCtbIdx, &ctbIdxKey, sizeof(ctbIdxKey), ctbEntry.ctbEntry.pTags,
                  ((STag *)(ctbEntry.ctbEntry.pTags))->len, pMeta->txn) < 0) {
    metaError("meta/table: failed to upsert ctb idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUidCacheClear(pMeta, ctbEntry.ctbEntry.suid) < 0) {
    metaError("meta/table: failed to clear uid cache:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaTbGroupCacheClear(pMeta, ctbEntry.ctbEntry.suid) < 0) {
    metaError("meta/table: failed to clear group cache:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUpdateChangeTime(pMeta, ctbEntry.uid, pAlterTbReq->ctimeMs) < 0) {
    metaError("meta/table: failed to update change time:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  metaULock(pMeta);

  tDecoderClear(&dc1);
  tDecoderClear(&dc2);
  taosMemoryFree((void *)ctbEntry.ctbEntry.pTags);
  if (ctbEntry.pBuf) taosMemoryFree(ctbEntry.pBuf);
  if (stbEntry.pBuf) tdbFree(stbEntry.pBuf);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  taosHashCleanup(pTagTable);
  return 0;

_err:
  tDecoderClear(&dc1);
  tDecoderClear(&dc2);
  if (ctbEntry.pBuf) taosMemoryFree(ctbEntry.pBuf);
  if (stbEntry.pBuf) tdbFree(stbEntry.pBuf);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  taosHashCleanup(pTagTable);
  return -1;
}
static int metaUpdateTableTagVal(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq) {
  SMetaEntry  ctbEntry = {0};
  SMetaEntry  stbEntry = {0};
  void       *pVal = NULL;
  int         nVal = 0;
  int         ret;
  int         c;
  tb_uid_t    uid;
  int64_t     oversion;
  const void *pData = NULL;
  int         nData = 0;

  if (pAlterTbReq->tagName == NULL) {
    return terrno = TSDB_CODE_INVALID_MSG;
  }

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  uid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  // search uid index
  TBC *pUidIdxc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pUidIdx, &pUidIdxc, NULL));
  if (tdbTbcMoveTo(pUidIdxc, &uid, sizeof(uid), &c) < 0) {
    metaTrace("meta/table: failed to move to uid index, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: invalide c: %" PRId32 " update tb tag val failed.", c);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  if (tdbTbcGet(pUidIdxc, NULL, NULL, &pData, &nData) != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: failed to get uid index, uid:%" PRId64, uid);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  oversion = ((SUidIdxVal *)pData)[0].version;

  // search table.db
  TBC     *pTbDbc = NULL;
  SDecoder dc1 = {0};
  SDecoder dc2 = {0};

  /* get ctbEntry */
  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTbDb, &pTbDbc, NULL));
  if (tdbTbcMoveTo(pTbDbc, &((STbDbKey){.uid = uid, .version = oversion}), sizeof(STbDbKey), &c) != 0) {
    metaError("meta/table: failed to move to tb db, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    metaError("meta/table: invalide c: %" PRId32 " update tb tag val failed.", c);
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  if (tdbTbcGet(pTbDbc, NULL, NULL, &pData, &nData) != 0) {
    metaError("meta/table: failed to get tb db, uid:%" PRId64, uid);
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno = TSDB_CODE_INVALID_MSG;
  }

  if ((ctbEntry.pBuf = taosMemoryMalloc(nData)) == NULL) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno;
  }
  memcpy(ctbEntry.pBuf, pData, nData);
  tDecoderInit(&dc1, ctbEntry.pBuf, nData);
  ret = metaDecodeEntry(&dc1, &ctbEntry);
  if (ret < 0) {
    terrno = ret;
    goto _err;
  }

  /* get stbEntry*/
  if (tdbTbGet(pMeta->pUidIdx, &ctbEntry.ctbEntry.suid, sizeof(tb_uid_t), &pVal, &nVal) != 0) {
    metaError("meta/table: failed to get uid index, uid:%" PRId64, ctbEntry.ctbEntry.suid);
  }
  if (!pVal) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _err;
  }

  if (tdbTbGet(pMeta->pTbDb, &((STbDbKey){.uid = ctbEntry.ctbEntry.suid, .version = ((SUidIdxVal *)pVal)[0].version}),
               sizeof(STbDbKey), (void **)&stbEntry.pBuf, &nVal) != 0) {
    metaError("meta/table: failed to get tb db, uid:%" PRId64, ctbEntry.ctbEntry.suid);
  }
  tdbFree(pVal);
  tDecoderInit(&dc2, stbEntry.pBuf, nVal);
  ret = metaDecodeEntry(&dc2, &stbEntry);
  if (ret < 0) {
    terrno = ret;
    goto _err;
  }

  SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
  SSchema        *pColumn = NULL;
  int32_t         iCol = 0;

  for (;;) {
    pColumn = NULL;

    if (iCol >= pTagSchema->nCols) break;
    pColumn = &pTagSchema->pSchema[iCol];

    if (strcmp(pColumn->name, pAlterTbReq->tagName) == 0) break;
    iCol++;
  }

  if (pColumn == NULL) {
    terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
    goto _err;
  }

  ctbEntry.version = version;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    ctbEntry.ctbEntry.pTags = taosMemoryMalloc(pAlterTbReq->nTagVal);
    if (ctbEntry.ctbEntry.pTags == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    memcpy((void *)ctbEntry.ctbEntry.pTags, pAlterTbReq->pTagVal, pAlterTbReq->nTagVal);
  } else {
    const STag *pOldTag = (const STag *)ctbEntry.ctbEntry.pTags;
    STag       *pNewTag = NULL;
    SArray     *pTagArray = taosArrayInit(pTagSchema->nCols, sizeof(STagVal));
    if (!pTagArray) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
    for (int32_t i = 0; i < pTagSchema->nCols; i++) {
      SSchema *pCol = &pTagSchema->pSchema[i];
      if (iCol == i) {
        if (pAlterTbReq->isNull) {
          continue;
        }
        STagVal val = {0};
        val.type = pCol->type;
        val.cid = pCol->colId;
        if (IS_VAR_DATA_TYPE(pCol->type)) {
          val.pData = pAlterTbReq->pTagVal;
          val.nData = pAlterTbReq->nTagVal;
        } else {
          memcpy(&val.i64, pAlterTbReq->pTagVal, pAlterTbReq->nTagVal);
        }
        if (taosArrayPush(pTagArray, &val) == NULL) {
          terrno = TSDB_CODE_OUT_OF_MEMORY;
          taosArrayDestroy(pTagArray);
          goto _err;
        }
      } else {
        STagVal val = {.cid = pCol->colId};
        if (tTagGet(pOldTag, &val)) {
          if (taosArrayPush(pTagArray, &val) == NULL) {
            terrno = TSDB_CODE_OUT_OF_MEMORY;
            taosArrayDestroy(pTagArray);
            goto _err;
          }
        }
      }
    }
    if ((terrno = tTagNew(pTagArray, pTagSchema->version, false, &pNewTag)) < 0) {
      taosArrayDestroy(pTagArray);
      goto _err;
    }
    ctbEntry.ctbEntry.pTags = (uint8_t *)pNewTag;
    taosArrayDestroy(pTagArray);
  }

  metaWLock(pMeta);

  // save to table.db
  if (metaSaveToTbDb(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to save to tb db:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  // save to uid.idx
  if (metaUpdateUidIdx(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to update uid idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUpdateTagIdx(pMeta, &ctbEntry) < 0) {
    metaError("meta/table: failed to update tag idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  SCtbIdxKey ctbIdxKey = {.suid = ctbEntry.ctbEntry.suid, .uid = uid};
  if (tdbTbUpsert(pMeta->pCtbIdx, &ctbIdxKey, sizeof(ctbIdxKey), ctbEntry.ctbEntry.pTags,
                  ((STag *)(ctbEntry.ctbEntry.pTags))->len, pMeta->txn) < 0) {
    metaError("meta/table: failed to upsert ctb idx:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUidCacheClear(pMeta, ctbEntry.ctbEntry.suid) < 0) {
    metaError("meta/table: failed to clear uid cache:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaTbGroupCacheClear(pMeta, ctbEntry.ctbEntry.suid) < 0) {
    metaError("meta/table: failed to clear group cache:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  if (metaUpdateChangeTime(pMeta, ctbEntry.uid, pAlterTbReq->ctimeMs) < 0) {
    metaError("meta/table: failed to update change time:%s uid:%" PRId64, ctbEntry.name, ctbEntry.uid);
  }

  metaULock(pMeta);

  tDecoderClear(&dc1);
  tDecoderClear(&dc2);
  taosMemoryFree((void *)ctbEntry.ctbEntry.pTags);
  if (ctbEntry.pBuf) taosMemoryFree(ctbEntry.pBuf);
  if (stbEntry.pBuf) tdbFree(stbEntry.pBuf);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  return 0;

_err:
  tDecoderClear(&dc1);
  tDecoderClear(&dc2);
  if (ctbEntry.pBuf) taosMemoryFree(ctbEntry.pBuf);
  if (stbEntry.pBuf) tdbFree(stbEntry.pBuf);
  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  return -1;
}

static int metaUpdateTableOptions(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq) {
  void       *pVal = NULL;
  int         nVal = 0;
  const void *pData = NULL;
  int         nData = 0;
  int         ret = 0;
  tb_uid_t    uid;
  int64_t     oversion;
  SMetaEntry  entry = {0};
  int         c = 0;

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }

  uid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  // search uid index
  TBC *pUidIdxc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pUidIdx, &pUidIdxc, NULL));
  if (tdbTbcMoveTo(pUidIdxc, &uid, sizeof(uid), &c) < 0) {
    metaError("meta/table: failed to move to uid index, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    metaError("meta/table: invalide c: %" PRId32 " update tb options failed.", c);
    return TSDB_CODE_FAILED;
  }

  if (tdbTbcGet(pUidIdxc, NULL, NULL, &pData, &nData) < 0) {
    metaError("meta/table: failed to get uid index, uid:%" PRId64, uid);
  }
  oversion = ((SUidIdxVal *)pData)[0].version;

  // search table.db
  TBC *pTbDbc = NULL;

  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTbDb, &pTbDbc, NULL));
  if (tdbTbcMoveTo(pTbDbc, &((STbDbKey){.uid = uid, .version = oversion}), sizeof(STbDbKey), &c) < 0) {
    metaError("meta/table: failed to move to tb db, uid:%" PRId64, uid);
  }
  if (c != 0) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    metaError("meta/table: invalide c: %" PRId32 " update tb options failed.", c);
    return TSDB_CODE_FAILED;
  }

  if (tdbTbcGet(pTbDbc, NULL, NULL, &pData, &nData) < 0) {
    metaError("meta/table: failed to get tb db, uid:%" PRId64, uid);
  }

  // get table entry
  SDecoder dc = {0};
  if ((entry.pBuf = taosMemoryMalloc(nData)) == NULL) {
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    return terrno;
  }
  memcpy(entry.pBuf, pData, nData);
  tDecoderInit(&dc, entry.pBuf, nData);
  ret = metaDecodeEntry(&dc, &entry);
  if (ret != 0) {
    tDecoderClear(&dc);
    tdbTbcClose(pUidIdxc);
    tdbTbcClose(pTbDbc);
    metaError("meta/table: invalide ret: %" PRId32 " alt tb options failed.", ret);
    return TSDB_CODE_FAILED;
  }

  entry.version = version;
  metaWLock(pMeta);
  // build SMetaEntry
  if (entry.type == TSDB_CHILD_TABLE) {
    if (pAlterTbReq->updateTTL) {
      metaDeleteTtl(pMeta, &entry);
      entry.ctbEntry.ttlDays = pAlterTbReq->newTTL;
      metaUpdateTtl(pMeta, &entry);
    }
    if (pAlterTbReq->newCommentLen >= 0) {
      entry.ctbEntry.commentLen = pAlterTbReq->newCommentLen;
      entry.ctbEntry.comment = pAlterTbReq->newComment;
    }
  } else {
    if (pAlterTbReq->updateTTL) {
      metaDeleteTtl(pMeta, &entry);
      entry.ntbEntry.ttlDays = pAlterTbReq->newTTL;
      metaUpdateTtl(pMeta, &entry);
    }
    if (pAlterTbReq->newCommentLen >= 0) {
      entry.ntbEntry.commentLen = pAlterTbReq->newCommentLen;
      entry.ntbEntry.comment = pAlterTbReq->newComment;
    }
  }

  // save to table db
  if (metaSaveToTbDb(pMeta, &entry) < 0) {
    metaError("meta/table: failed to save to tb db:%s uid:%" PRId64, entry.name, entry.uid);
  }

  if (metaUpdateUidIdx(pMeta, &entry) < 0) {
    metaError("meta/table: failed to update uid idx:%s uid:%" PRId64, entry.name, entry.uid);
  }

  if (metaUpdateChangeTime(pMeta, entry.uid, pAlterTbReq->ctimeMs) < 0) {
    metaError("meta/table: failed to update change time:%s uid:%" PRId64, entry.name, entry.uid);
  }

  metaULock(pMeta);

  tdbTbcClose(pTbDbc);
  tdbTbcClose(pUidIdxc);
  tDecoderClear(&dc);
  if (entry.pBuf) taosMemoryFree(entry.pBuf);
  return 0;
}

static int metaAddTagIndex(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq) {
  SMetaEntry  stbEntry = {0};
  void       *pVal = NULL;
  int         nVal = 0;
  int         ret;
  int         c;
  tb_uid_t    uid, suid;
  int64_t     oversion;
  const void *pData = NULL;
  int         nData = 0;
  SDecoder    dc = {0};

  if (pAlterTbReq->tagName == NULL) {
    return terrno = TSDB_CODE_INVALID_MSG;
  }

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  } else {
    uid = *(tb_uid_t *)pVal;
    tdbFree(pVal);
    pVal = NULL;
  }

  if (tdbTbGet(pMeta->pUidIdx, &uid, sizeof(tb_uid_t), &pVal, &nVal) == -1) {
    ret = -1;
    goto _err;
  }
  suid = ((SUidIdxVal *)pVal)[0].suid;

  STbDbKey tbDbKey = {0};
  tbDbKey.uid = suid;
  tbDbKey.version = ((SUidIdxVal *)pVal)[0].version;
  ret = tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pVal, &nVal);
  if (ret < 0) {
    goto _err;
  }
  tDecoderInit(&dc, pVal, nVal);
  ret = metaDecodeEntry(&dc, &stbEntry);
  if (ret < 0) {
    tDecoderClear(&dc);
    goto _err;
  }

  // Get target schema info
  SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    terrno = TSDB_CODE_VND_COL_ALREADY_EXISTS;
    goto _err;
  }
  SSchema *pCol = NULL;
  int32_t  iCol = 0;
  for (;;) {
    pCol = NULL;
    if (iCol >= pTagSchema->nCols) break;
    pCol = &pTagSchema->pSchema[iCol];
    if (strcmp(pCol->name, pAlterTbReq->tagName) == 0) break;
    iCol++;
  }

  if (iCol == 0) {
    terrno = TSDB_CODE_VND_COL_ALREADY_EXISTS;
    goto _err;
  }
  if (pCol == NULL) {
    terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
    goto _err;
  }

  /*
   * iterator all pTdDbc by uid and version
   */
  TBC *pCtbIdxc = NULL;
  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pCtbIdx, &pCtbIdxc, NULL));
  int rc = tdbTbcMoveTo(pCtbIdxc, &(SCtbIdxKey){.suid = suid, .uid = INT64_MIN}, sizeof(SCtbIdxKey), &c);
  if (rc < 0) {
    tdbTbcClose(pCtbIdxc);
    goto _err;
  }
  for (;;) {
    void *pKey, *pVal;
    int   nKey, nVal;
    rc = tdbTbcNext(pCtbIdxc, &pKey, &nKey, &pVal, &nVal);
    if (rc < 0) break;
    if (((SCtbIdxKey *)pKey)->suid != uid) {
      tdbFree(pKey);
      tdbFree(pVal);
      continue;
    }
    STagIdxKey *pTagIdxKey = NULL;
    int32_t     nTagIdxKey;

    const void *pTagData = NULL;
    int32_t     nTagData = 0;

    STagVal tagVal = {.cid = pCol->colId};
    if (tTagGet((const STag *)pVal, &tagVal)) {
      if (IS_VAR_DATA_TYPE(pCol->type)) {
        pTagData = tagVal.pData;
        nTagData = (int32_t)tagVal.nData;
      } else {
        pTagData = &(tagVal.i64);
        nTagData = tDataTypes[pCol->type].bytes;
      }
    } else {
      if (!IS_VAR_DATA_TYPE(pCol->type)) {
        nTagData = tDataTypes[pCol->type].bytes;
      }
    }
    if (metaCreateTagIdxKey(suid, pCol->colId, pTagData, nTagData, pCol->type, uid, &pTagIdxKey, &nTagIdxKey) < 0) {
      tdbFree(pKey);
      tdbFree(pVal);
      metaDestroyTagIdxKey(pTagIdxKey);
      tdbTbcClose(pCtbIdxc);
      goto _err;
    }
    ret = tdbTbUpsert(pMeta->pTagIdx, pTagIdxKey, nTagIdxKey, NULL, 0, pMeta->txn);
    if (ret < 0) {
      metaError("meta/table: failed to upsert tag idx:%s uid:%" PRId64, stbEntry.name, stbEntry.uid);
    }
    metaDestroyTagIdxKey(pTagIdxKey);
    pTagIdxKey = NULL;
  }
  tdbTbcClose(pCtbIdxc);
  return 0;

_err:
  // tDecoderClear(&dc1);
  // tDecoderClear(&dc2);
  // if (ctbEntry.pBuf) taosMemoryFree(ctbEntry.pBuf);
  // if (stbEntry.pBuf) tdbFree(stbEntry.pBuf);
  // tdbTbcClose(pTbDbc);
  // tdbTbcClose(pUidIdxc);
  return TSDB_CODE_FAILED;
}

typedef struct SMetaPair {
  void *key;
  int   nkey;
} SMetaPair;

static int metaDropTagIndex(SMeta *pMeta, int64_t version, SVAlterTbReq *pAlterTbReq) {
  SMetaEntry  stbEntry = {0};
  void       *pVal = NULL;
  int         nVal = 0;
  int         ret;
  int         c;
  tb_uid_t    suid;
  int64_t     oversion;
  const void *pData = NULL;
  int         nData = 0;
  SDecoder    dc = {0};

  if (pAlterTbReq->tagName == NULL) {
    return terrno = TSDB_CODE_INVALID_MSG;
  }

  // search name index
  ret = tdbTbGet(pMeta->pNameIdx, pAlterTbReq->tbName, strlen(pAlterTbReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  suid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  if (tdbTbGet(pMeta->pUidIdx, &suid, sizeof(tb_uid_t), &pVal, &nVal) == -1) {
    ret = -1;
    goto _err;
  }

  STbDbKey tbDbKey = {0};
  tbDbKey.uid = suid;
  tbDbKey.version = ((SUidIdxVal *)pVal)[0].version;
  ret = tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pVal, &nVal);
  if (ret < 0) {
    goto _err;
  }

  tDecoderInit(&dc, pVal, nVal);
  ret = metaDecodeEntry(&dc, &stbEntry);
  if (ret < 0) {
    tDecoderClear(&dc);
    goto _err;
  }

  // Get targe schema info
  SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    terrno = TSDB_CODE_VND_COL_ALREADY_EXISTS;
    goto _err;
  }
  SSchema *pCol = NULL;
  int32_t  iCol = 0;
  for (;;) {
    pCol = NULL;
    if (iCol >= pTagSchema->nCols) break;
    pCol = &pTagSchema->pSchema[iCol];
    if (strcmp(pCol->name, pAlterTbReq->tagName) == 0) break;
    iCol++;
  }
  if (iCol == 0) {
    // cannot drop 1th tag index
    terrno = -1;
    goto _err;
  }
  if (pCol == NULL) {
    terrno = TSDB_CODE_VND_COL_NOT_EXISTS;
    goto _err;
  }

  if (IS_IDX_ON(pCol)) {
    terrno = TSDB_CODE_VND_COL_ALREADY_EXISTS;
    goto _err;
  }

  SArray *tagIdxList = taosArrayInit(512, sizeof(SMetaPair));
  if (tagIdxList == NULL) {
    goto _err;
  }

  TBC *pTagIdxc = NULL;
  TAOS_CHECK_RETURN(tdbTbcOpen(pMeta->pTagIdx, &pTagIdxc, NULL));
  int rc =
      tdbTbcMoveTo(pTagIdxc, &(STagIdxKey){.suid = suid, .cid = INT32_MIN, .type = pCol->type}, sizeof(STagIdxKey), &c);
  for (;;) {
    void *pKey, *pVal;
    int   nKey, nVal;
    rc = tdbTbcNext(pTagIdxc, &pKey, &nKey, &pVal, &nVal);
    STagIdxKey *pIdxKey = (STagIdxKey *)pKey;
    if (pIdxKey->suid != suid || pIdxKey->cid != pCol->colId) {
      tdbFree(pKey);
      tdbFree(pVal);
      continue;
    }

    SMetaPair pair = {.key = pKey, nKey = nKey};
    if (taosArrayPush(tagIdxList, &pair) == NULL) {
      goto _err;
    }
  }
  tdbTbcClose(pTagIdxc);

  metaWLock(pMeta);
  for (int i = 0; i < taosArrayGetSize(tagIdxList); i++) {
    SMetaPair *pair = taosArrayGet(tagIdxList, i);
    ret = tdbTbDelete(pMeta->pTagIdx, pair->key, pair->nkey, pMeta->txn);
    if (ret < 0) {
      metaError("meta/table: failed to delete tag idx:%s uid:%" PRId64, stbEntry.name, stbEntry.uid);
    }
  }
  metaULock(pMeta);

  taosArrayDestroy(tagIdxList);

  // set pCol->flags; INDEX_ON
  return 0;
_err:
  return TSDB_CODE_FAILED;
}
int32_t metaUpdateTableColCompress(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq) {
  // impl later
  SMetaEntry  tbEntry = {0};
  void       *pVal = NULL;
  int         nVal = 0;
  int         ret;
  int         c;
  tb_uid_t    suid;
  int64_t     oversion;
  const void *pData = NULL;
  int         nData = 0;
  SDecoder    dc = {0};
  ret = tdbTbGet(pMeta->pNameIdx, pReq->tbName, strlen(pReq->tbName) + 1, &pVal, &nVal);
  if (ret < 0) {
    return terrno = TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  suid = *(tb_uid_t *)pVal;
  tdbFree(pVal);
  pVal = NULL;

  if (tdbTbGet(pMeta->pUidIdx, &suid, sizeof(tb_uid_t), &pVal, &nVal) == -1) {
    terrno = TSDB_CODE_INVALID_MSG;
    ret = -1;
    goto _err;
  }

  STbDbKey tbDbKey = {0};
  tbDbKey.uid = suid;
  tbDbKey.version = ((SUidIdxVal *)pVal)[0].version;
  if (tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pVal, &nVal) < 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    tdbFree(pVal);
    goto _err;
  }

  tDecoderInit(&dc, pVal, nVal);
  ret = metaDecodeEntry(&dc, &tbEntry);
  if (ret < 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    tdbFree(pVal);
    tDecoderClear(&dc);
    goto _err;
  }
  if (tbEntry.type != TSDB_NORMAL_TABLE && tbEntry.type != TSDB_SUPER_TABLE) {
    terrno = TSDB_CODE_INVALID_MSG;
    tdbFree(pVal);
    tDecoderClear(&dc);
    goto _err;
  }
  int8_t           updated = 0;
  SColCmprWrapper *wp = &tbEntry.colCmpr;
  for (int32_t i = 0; i < wp->nCols; i++) {
    SColCmpr *p = &wp->pColCmpr[i];
    if (p->id == pReq->colId) {
      uint32_t dst = 0;
      updated = tUpdateCompress(p->alg, pReq->compress, TSDB_COLVAL_COMPRESS_DISABLED, TSDB_COLVAL_LEVEL_DISABLED,
                                TSDB_COLVAL_LEVEL_MEDIUM, &dst);
      if (updated > 0) {
        p->alg = dst;
      }
    }
  }
  if (updated == 0) {
    tdbFree(pVal);
    tDecoderClear(&dc);
    terrno = TSDB_CODE_VND_COLUMN_COMPRESS_ALREADY_EXIST;
    goto _err;
  } else if (updated < 0) {
    tdbFree(pVal);
    tDecoderClear(&dc);
    terrno = TSDB_CODE_TSC_COMPRESS_LEVEL_ERROR;
    goto _err;
  }
  tbEntry.version = version;

  metaWLock(pMeta);
  if (metaSaveToTbDb(pMeta, &tbEntry) < 0) {
    metaError("meta/table: failed to save to tb db:%s uid:%" PRId64, tbEntry.name, tbEntry.uid);
  }

  if (metaUpdateUidIdx(pMeta, &tbEntry) < 0) {
    metaError("meta/table: failed to update uid idx:%s uid:%" PRId64, tbEntry.name, tbEntry.uid);
  }

  if (metaUpdateChangeTime(pMeta, suid, pReq->ctimeMs) < 0) {
    metaError("meta/table: failed to update change time:%s uid:%" PRId64, tbEntry.name, tbEntry.uid);
  }

  metaULock(pMeta);

  tdbFree(pVal);
  tDecoderClear(&dc);

  return 0;
_err:
  return TSDB_CODE_FAILED;
}

int metaAlterTable(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pMetaRsp) {
  pMeta->changed = true;
  switch (pReq->action) {
    case TSDB_ALTER_TABLE_ADD_COLUMN:
    case TSDB_ALTER_TABLE_ADD_COLUMN_WITH_COMPRESS_OPTION:
      return metaAddTableColumn(pMeta, version, pReq, pMetaRsp);
    case TSDB_ALTER_TABLE_DROP_COLUMN:
      return metaDropTableColumn(pMeta, version, pReq, pMetaRsp);
    case TSDB_ALTER_TABLE_UPDATE_COLUMN_BYTES:
      return metaAlterTableColumnBytes(pMeta, version, pReq, pMetaRsp);
    case TSDB_ALTER_TABLE_UPDATE_COLUMN_NAME:
      return metaAlterTableColumnName(pMeta, version, pReq, pMetaRsp);
    case TSDB_ALTER_TABLE_UPDATE_TAG_VAL:
      return metaUpdateTableTagValue(pMeta, version, pReq);
    case TSDB_ALTER_TABLE_UPDATE_MULTI_TAG_VAL:
      return metaUpdateTableMultiTagValue(pMeta, version, pReq);
    case TSDB_ALTER_TABLE_UPDATE_OPTIONS:
      return metaUpdateTableOptions2(pMeta, version, pReq);
    case TSDB_ALTER_TABLE_ADD_TAG_INDEX:
      return metaAddTagIndex(pMeta, version, pReq);
    case TSDB_ALTER_TABLE_DROP_TAG_INDEX:
      return metaDropTagIndex(pMeta, version, pReq);
    case TSDB_ALTER_TABLE_UPDATE_COLUMN_COMPRESS:
      return metaUpdateTableColCompress(pMeta, version, pReq);
    default:
      return terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
      break;
  }
}

static int metaSaveToTbDb(SMeta *pMeta, const SMetaEntry *pME) {
  STbDbKey tbDbKey;
  void    *pKey = NULL;
  void    *pVal = NULL;
  int      kLen = 0;
  int      vLen = 0;
  SEncoder coder = {0};

  // set key and value
  tbDbKey.version = pME->version;
  tbDbKey.uid = pME->uid;

  metaDebug("vgId:%d, start to save table version:%" PRId64 " uid:%" PRId64, TD_VID(pMeta->pVnode), pME->version,
            pME->uid);

  pKey = &tbDbKey;
  kLen = sizeof(tbDbKey);

  int32_t ret = 0;
  tEncodeSize(metaEncodeEntry, pME, vLen, ret);
  if (ret < 0) {
    goto _err;
  }

  pVal = taosMemoryMalloc(vLen);
  if (pVal == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  tEncoderInit(&coder, pVal, vLen);

  if (metaEncodeEntry(&coder, pME) < 0) {
    goto _err;
  }

  tEncoderClear(&coder);

  // write to table.db
  if (tdbTbInsert(pMeta->pTbDb, pKey, kLen, pVal, vLen, pMeta->txn) < 0) {
    goto _err;
  }

  taosMemoryFree(pVal);
  return 0;

_err:
  metaError("vgId:%d, failed to save table version:%" PRId64 "uid:%" PRId64 " %s", TD_VID(pMeta->pVnode), pME->version,
            pME->uid, tstrerror(terrno));

  taosMemoryFree(pVal);
  return TSDB_CODE_FAILED;
}

static int metaUpdateUidIdx(SMeta *pMeta, const SMetaEntry *pME) {
  // upsert cache
  SMetaInfo info;
  metaGetEntryInfo(pME, &info);
  int32_t ret = metaCacheUpsert(pMeta, &info);
  if (ret < 0) {
    metaError("vgId:%d, failed to upsert cache, uid: %" PRId64 " %s", TD_VID(pMeta->pVnode), pME->uid, tstrerror(ret));
  }

  SUidIdxVal uidIdxVal = {.suid = info.suid, .version = info.version, .skmVer = info.skmVer};

  return tdbTbUpsert(pMeta->pUidIdx, &pME->uid, sizeof(tb_uid_t), &uidIdxVal, sizeof(uidIdxVal), pMeta->txn);
}

static int metaUpdateSuidIdx(SMeta *pMeta, const SMetaEntry *pME) {
  return tdbTbUpsert(pMeta->pSuidIdx, &pME->uid, sizeof(tb_uid_t), NULL, 0, pMeta->txn);
}

static int metaUpdateNameIdx(SMeta *pMeta, const SMetaEntry *pME) {
  return tdbTbUpsert(pMeta->pNameIdx, pME->name, strlen(pME->name) + 1, &pME->uid, sizeof(tb_uid_t), pMeta->txn);
}

static void metaUpdateTtl(SMeta *pMeta, const SMetaEntry *pME) {
  if (pME->type != TSDB_CHILD_TABLE && pME->type != TSDB_NORMAL_TABLE) return;

  STtlUpdTtlCtx ctx = {.uid = pME->uid, .pTxn = pMeta->txn};
  if (pME->type == TSDB_CHILD_TABLE) {
    ctx.ttlDays = pME->ctbEntry.ttlDays;
    ctx.changeTimeMs = pME->ctbEntry.btime;
  } else {
    ctx.ttlDays = pME->ntbEntry.ttlDays;
    ctx.changeTimeMs = pME->ntbEntry.btime;
  }

  int32_t ret = ttlMgrInsertTtl(pMeta->pTtlMgr, &ctx);
  if (ret < 0) {
    metaError("vgId:%d, failed to insert ttl, uid: %" PRId64 " %s", TD_VID(pMeta->pVnode), pME->uid, tstrerror(ret));
  }

  return;
}

static int metaUpdateChangeTime(SMeta *pMeta, tb_uid_t uid, int64_t changeTimeMs) {
  if (!tsTtlChangeOnWrite) return 0;

  if (changeTimeMs <= 0) {
    metaWarn("Skip to change ttl deletetion time on write, uid: %" PRId64, uid);
    return TSDB_CODE_VERSION_NOT_COMPATIBLE;
  }

  STtlUpdCtimeCtx ctx = {.uid = uid, .changeTimeMs = changeTimeMs, .pTxn = pMeta->txn};

  return ttlMgrUpdateChangeTime(pMeta->pTtlMgr, &ctx);
}

int metaUpdateChangeTimeWithLock(SMeta *pMeta, tb_uid_t uid, int64_t changeTimeMs) {
  if (!tsTtlChangeOnWrite) return 0;

  metaWLock(pMeta);
  int ret = metaUpdateChangeTime(pMeta, uid, changeTimeMs);
  metaULock(pMeta);
  return ret;
}

static int metaUpdateCtbIdx(SMeta *pMeta, const SMetaEntry *pME) {
  SCtbIdxKey ctbIdxKey = {.suid = pME->ctbEntry.suid, .uid = pME->uid};

  return tdbTbUpsert(pMeta->pCtbIdx, &ctbIdxKey, sizeof(ctbIdxKey), pME->ctbEntry.pTags,
                     ((STag *)(pME->ctbEntry.pTags))->len, pMeta->txn);
}

int metaCreateTagIdxKey(tb_uid_t suid, int32_t cid, const void *pTagData, int32_t nTagData, int8_t type, tb_uid_t uid,
                        STagIdxKey **ppTagIdxKey, int32_t *nTagIdxKey) {
  if (IS_VAR_DATA_TYPE(type)) {
    *nTagIdxKey = sizeof(STagIdxKey) + nTagData + VARSTR_HEADER_SIZE + sizeof(tb_uid_t);
  } else {
    *nTagIdxKey = sizeof(STagIdxKey) + nTagData + sizeof(tb_uid_t);
  }

  *ppTagIdxKey = (STagIdxKey *)taosMemoryMalloc(*nTagIdxKey);
  if (*ppTagIdxKey == NULL) {
    return terrno;
  }

  (*ppTagIdxKey)->suid = suid;
  (*ppTagIdxKey)->cid = cid;
  (*ppTagIdxKey)->isNull = (pTagData == NULL) ? 1 : 0;
  (*ppTagIdxKey)->type = type;

  // refactor
  if (IS_VAR_DATA_TYPE(type)) {
    memcpy((*ppTagIdxKey)->data, (uint16_t *)&nTagData, VARSTR_HEADER_SIZE);
    if (pTagData != NULL) memcpy((*ppTagIdxKey)->data + VARSTR_HEADER_SIZE, pTagData, nTagData);
    *(tb_uid_t *)((*ppTagIdxKey)->data + VARSTR_HEADER_SIZE + nTagData) = uid;
  } else {
    if (pTagData != NULL) memcpy((*ppTagIdxKey)->data, pTagData, nTagData);
    *(tb_uid_t *)((*ppTagIdxKey)->data + nTagData) = uid;
  }

  return 0;
}

void metaDestroyTagIdxKey(STagIdxKey *pTagIdxKey) {
  if (pTagIdxKey) taosMemoryFree(pTagIdxKey);
}

static int metaUpdateTagIdx(SMeta *pMeta, const SMetaEntry *pCtbEntry) {
  void          *pData = NULL;
  int            nData = 0;
  STbDbKey       tbDbKey = {0};
  SMetaEntry     stbEntry = {0};
  STagIdxKey    *pTagIdxKey = NULL;
  int32_t        nTagIdxKey;
  const SSchema *pTagColumn;
  const void    *pTagData = NULL;
  int32_t        nTagData = 0;
  SDecoder       dc = {0};
  int32_t        ret = 0;
  // get super table
  if (tdbTbGet(pMeta->pUidIdx, &pCtbEntry->ctbEntry.suid, sizeof(tb_uid_t), &pData, &nData) != 0) {
    metaError("vgId:%d, failed to get stable suid for update. version:%" PRId64, TD_VID(pMeta->pVnode),
              pCtbEntry->version);
    ret = TSDB_CODE_TDB_INVALID_TABLE_ID;
    goto end;
  }
  tbDbKey.uid = pCtbEntry->ctbEntry.suid;
  tbDbKey.version = ((SUidIdxVal *)pData)[0].version;
  ret = tdbTbGet(pMeta->pTbDb, &tbDbKey, sizeof(tbDbKey), &pData, &nData);
  if (ret < 0) {
    metaError("vgId:%d, failed to get stable for update. version:%" PRId64, TD_VID(pMeta->pVnode), pCtbEntry->version);
    goto end;
  }

  tDecoderInit(&dc, pData, nData);
  ret = metaDecodeEntry(&dc, &stbEntry);
  if (ret < 0) {
    goto end;
  }

  if (stbEntry.stbEntry.schemaTag.pSchema == NULL) {
    ret = TSDB_CODE_INVALID_PARA;
    goto end;
  }

  SSchemaWrapper *pTagSchema = &stbEntry.stbEntry.schemaTag;
  if (pTagSchema->nCols == 1 && pTagSchema->pSchema[0].type == TSDB_DATA_TYPE_JSON) {
    pTagColumn = &stbEntry.stbEntry.schemaTag.pSchema[0];
    STagVal tagVal = {.cid = pTagColumn->colId};

    pTagData = pCtbEntry->ctbEntry.pTags;
    nTagData = ((const STag *)pCtbEntry->ctbEntry.pTags)->len;
    ret = metaSaveJsonVarToIdx(pMeta, pCtbEntry, pTagColumn);
    goto end;
  } else {
    for (int i = 0; i < pTagSchema->nCols; i++) {
      pTagData = NULL;
      nTagData = 0;
      pTagColumn = &pTagSchema->pSchema[i];
      if (!IS_IDX_ON(pTagColumn)) continue;

      STagVal tagVal = {.cid = pTagColumn->colId};
      if (tTagGet((const STag *)pCtbEntry->ctbEntry.pTags, &tagVal)) {
        if (IS_VAR_DATA_TYPE(pTagColumn->type)) {
          pTagData = tagVal.pData;
          nTagData = (int32_t)tagVal.nData;
        } else {
          pTagData = &(tagVal.i64);
          nTagData = tDataTypes[pTagColumn->type].bytes;
        }
      } else {
        if (!IS_VAR_DATA_TYPE(pTagColumn->type)) {
          nTagData = tDataTypes[pTagColumn->type].bytes;
        }
      }
      if (metaCreateTagIdxKey(pCtbEntry->ctbEntry.suid, pTagColumn->colId, pTagData, nTagData, pTagColumn->type,
                              pCtbEntry->uid, &pTagIdxKey, &nTagIdxKey) < 0) {
        ret = -1;
        goto end;
      }
      if (tdbTbUpsert(pMeta->pTagIdx, pTagIdxKey, nTagIdxKey, NULL, 0, pMeta->txn) < 0) {
        metaError("vgId:%d, failed to update tag index. version:%" PRId64, TD_VID(pMeta->pVnode), pCtbEntry->version);
      }
      metaDestroyTagIdxKey(pTagIdxKey);
      pTagIdxKey = NULL;
    }
  }
end:
  tDecoderClear(&dc);
  tdbFree(pData);
  return ret;
}

static int metaSaveToSkmDb(SMeta *pMeta, const SMetaEntry *pME) {
  SEncoder              coder = {0};
  void                 *pVal = NULL;
  int                   vLen = 0;
  int                   rcode = 0;
  SSkmDbKey             skmDbKey = {0};
  const SSchemaWrapper *pSW;

  if (pME->type == TSDB_SUPER_TABLE) {
    pSW = &pME->stbEntry.schemaRow;
  } else if (pME->type == TSDB_NORMAL_TABLE) {
    pSW = &pME->ntbEntry.schemaRow;
  } else {
    metaError("meta/table: invalide table type: %" PRId8 " save skm db failed.", pME->type);
    return TSDB_CODE_FAILED;
  }

  skmDbKey.uid = pME->uid;
  skmDbKey.sver = pSW->version;

  // if receive tmq meta message is: create stable1 then delete stable1 then create stable1 with multi vgroups
  if (tdbTbGet(pMeta->pSkmDb, &skmDbKey, sizeof(skmDbKey), NULL, NULL) == 0) {
    return rcode;
  }

  // encode schema
  int32_t ret = 0;
  tEncodeSize(tEncodeSSchemaWrapper, pSW, vLen, ret);
  if (ret < 0) return -1;
  pVal = taosMemoryMalloc(vLen);
  if (pVal == NULL) {
    rcode = -1;
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  tEncoderInit(&coder, pVal, vLen);
  ret = tEncodeSSchemaWrapper(&coder, pSW);
  if (ret < 0) {
    rcode = -1;
    goto _exit;
  }

  if (tdbTbInsert(pMeta->pSkmDb, &skmDbKey, sizeof(skmDbKey), pVal, vLen, pMeta->txn) < 0) {
    rcode = -1;
    goto _exit;
  }

  metaDebug("vgId:%d, set schema:(%" PRId64 ") sver:%d since %s", TD_VID(pMeta->pVnode), pME->uid, pSW->version,
            tstrerror(terrno));

_exit:
  taosMemoryFree(pVal);
  tEncoderClear(&coder);
  return rcode;
}

int metaHandleEntry(SMeta *pMeta, const SMetaEntry *pME) {
  int32_t code = 0;
  int32_t line = 0;
  metaWLock(pMeta);

  // save to table.db
  code = metaSaveToTbDb(pMeta, pME);
  VND_CHECK_CODE(code, line, _err);

  // update uid.idx
  code = metaUpdateUidIdx(pMeta, pME);
  VND_CHECK_CODE(code, line, _err);

  // update name.idx
  code = metaUpdateNameIdx(pMeta, pME);
  VND_CHECK_CODE(code, line, _err);

  if (pME->type == TSDB_CHILD_TABLE) {
    // update ctb.idx
    code = metaUpdateCtbIdx(pMeta, pME);
    VND_CHECK_CODE(code, line, _err);

    // update tag.idx
    code = metaUpdateTagIdx(pMeta, pME);
    VND_CHECK_CODE(code, line, _err);
  } else {
    // update schema.db
    code = metaSaveToSkmDb(pMeta, pME);
    VND_CHECK_CODE(code, line, _err);

    if (pME->type == TSDB_SUPER_TABLE) {
      code = metaUpdateSuidIdx(pMeta, pME);
      VND_CHECK_CODE(code, line, _err);
    }
  }

  code = metaUpdateBtimeIdx(pMeta, pME);
  VND_CHECK_CODE(code, line, _err);

  if (pME->type == TSDB_NORMAL_TABLE) {
    code = metaUpdateNcolIdx(pMeta, pME);
    VND_CHECK_CODE(code, line, _err);
  }

  if (pME->type != TSDB_SUPER_TABLE) {
    metaUpdateTtl(pMeta, pME);
  }

  if (pME->type == TSDB_SUPER_TABLE || pME->type == TSDB_NORMAL_TABLE) {
  }

  metaULock(pMeta);
  metaDebug("vgId:%d, handle meta entry, ver:%" PRId64 ", uid:%" PRId64 ", name:%s", TD_VID(pMeta->pVnode),
            pME->version, pME->uid, pME->name);
  return 0;

_err:
  metaULock(pMeta);
  metaError("vgId:%d, failed to handle meta entry since %s at line:%d, ver:%" PRId64 ", uid:%" PRId64 ", name:%s",
            TD_VID(pMeta->pVnode), terrstr(), line, pME->version, pME->uid, pME->name);
  return TSDB_CODE_FAILED;
}

static void colCompressDebug(SHashObj *pColCmprObj) {
  void *p = taosHashIterate(pColCmprObj, NULL);
  while (p) {
    uint32_t cmprAlg = *(uint32_t *)p;
    col_id_t colId = *(col_id_t *)taosHashGetKey(p, NULL);
    p = taosHashIterate(pColCmprObj, p);

    uint8_t l1, l2, lvl;
    tcompressDebug(cmprAlg, &l1, &l2, &lvl);

    const char *l1str = columnEncodeStr(l1);
    const char *l2str = columnCompressStr(l2);
    const char *lvlstr = columnLevelStr(lvl);
    metaDebug("colId: %d, encode:%s, compress:%s,level:%s", colId, l1str, l2str, lvlstr);
  }
  return;
}
int32_t metaGetColCmpr(SMeta *pMeta, tb_uid_t uid, SHashObj **ppColCmprObj) {
  int rc = 0;

  SHashObj *pColCmprObj = taosHashInit(32, taosGetDefaultHashFunction(TSDB_DATA_TYPE_SMALLINT), false, HASH_NO_LOCK);
  if (pColCmprObj == NULL) {
    pColCmprObj = NULL;
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  void      *pData = NULL;
  int        nData = 0;
  SMetaEntry e = {0};
  SDecoder   dc = {0};

  *ppColCmprObj = NULL;

  metaRLock(pMeta);
  rc = tdbTbGet(pMeta->pUidIdx, &uid, sizeof(uid), &pData, &nData);
  if (rc < 0) {
    taosHashClear(pColCmprObj);
    metaULock(pMeta);
    return TSDB_CODE_FAILED;
  }
  int64_t version = ((SUidIdxVal *)pData)[0].version;
  rc = tdbTbGet(pMeta->pTbDb, &(STbDbKey){.version = version, .uid = uid}, sizeof(STbDbKey), &pData, &nData);
  if (rc < 0) {
    metaULock(pMeta);
    taosHashClear(pColCmprObj);
    metaError("failed to get table entry");
    return rc;
  }

  tDecoderInit(&dc, pData, nData);
  rc = metaDecodeEntry(&dc, &e);
  if (rc < 0) {
    tDecoderClear(&dc);
    tdbFree(pData);
    metaULock(pMeta);
    taosHashClear(pColCmprObj);
    return rc;
  }
  if (useCompress(e.type)) {
    SColCmprWrapper *p = &e.colCmpr;
    for (int32_t i = 0; i < p->nCols; i++) {
      SColCmpr *pCmpr = &p->pColCmpr[i];
      rc = taosHashPut(pColCmprObj, &pCmpr->id, sizeof(pCmpr->id), &pCmpr->alg, sizeof(pCmpr->alg));
      if (rc < 0) {
        tDecoderClear(&dc);
        tdbFree(pData);
        metaULock(pMeta);
        taosHashClear(pColCmprObj);
        return rc;
      }
    }
  } else {
    tDecoderClear(&dc);
    tdbFree(pData);
    metaULock(pMeta);
    taosHashClear(pColCmprObj);
    return 0;
  }
  tDecoderClear(&dc);
  tdbFree(pData);
  metaULock(pMeta);

  *ppColCmprObj = pColCmprObj;
  colCompressDebug(pColCmprObj);

  return 0;
}
// refactor later
void *metaGetIdx(SMeta *pMeta) { return pMeta->pTagIdx; }
void *metaGetIvtIdx(SMeta *pMeta) { return pMeta->pTagIvtIdx; }
