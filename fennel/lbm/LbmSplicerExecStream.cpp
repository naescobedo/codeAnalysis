/*
// Licensed to DynamoBI Corporation (DynamoBI) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  DynamoBI licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at

//   http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
*/

#include "fennel/common/CommonPreamble.h"
#include "fennel/common/FennelResource.h"
#include "fennel/segment/SegmentFactory.h"
#include "fennel/btree/BTreeBuilder.h"
#include "fennel/exec/ExecStreamBufAccessor.h"
#include "fennel/lbm/LbmGeneratorExecStream.h"
#include "fennel/lbm/LbmSplicerExecStream.h"

FENNEL_BEGIN_CPPFILE("$Id$");

void LbmSplicerExecStream::prepare(LbmSplicerExecStreamParams const &params)
{
    DiffluenceExecStream::prepare(params);
    scratchAccessor = params.scratchAccessor;

    // Setup btree accessed by splicer
    assert(params.bTreeParams.size() <= 2);
    assert(params.bTreeParams[0].pRootMap == NULL);
    BTreeExecStream::copyParamsToDescriptor(
        writeBTreeDesc,
        params.bTreeParams[0],
        params.pCacheAccessor);

    insertRowCountParamId = params.insertRowCountParamId;
    computeRowCount = (opaqueToInt(insertRowCountParamId) == 0);
    writeRowCountParamId = params.writeRowCountParamId;

    bitmapTupleDesc = writeBTreeDesc.tupleDescriptor;
    bTreeTupleData.compute(bitmapTupleDesc);
    tempBTreeTupleData.compute(bitmapTupleDesc);
    inputTuple.compute(bitmapTupleDesc);
    nIdxKeys = writeBTreeDesc.keyProjection.size() - 1;

    // if the rowcount needs to be computed, then the input contains singleton
    // rids; so setup a special tupleData to receive that input
    if (computeRowCount) {
        assert(nIdxKeys == 0);
        assert(pInAccessor->getTupleDesc().size() == 1);
        singletonTuple.compute(pInAccessor->getTupleDesc());
    } else {
        assert(
            writeBTreeDesc.tupleDescriptor == pInAccessor->getTupleDesc());
    }

    uint minEntrySize;
    LbmEntry::getSizeBounds(
        bitmapTupleDesc,
        writeBTreeDesc.segmentAccessor.pSegment->getUsablePageSize(),
        minEntrySize,
        maxEntrySize);

    // setup output tuple
    outputTuple.compute(outAccessors[0]->getTupleDesc());
    outputTuple[0].pData = (PConstBuffer) &numRowsLoaded;
    assert(outputTuple.size() == 1);

    // constraint checking
    uniqueKey = false;
    if (params.bTreeParams.size() >= 2) {
        uniqueKey = true;
        BTreeExecStream::copyParamsToDescriptor(
            deletionBTreeDesc,
            params.bTreeParams[1],
            params.pCacheAccessor);
        deletionTuple.compute(deletionBTreeDesc.tupleDescriptor);

        TupleDescriptor currUniqueKeyDesc;
        for (uint i = 0; i < nIdxKeys; i++) {
            currUniqueKeyDesc.push_back(bitmapTupleDesc[i]);
        }
        currUniqueKey.computeAndAllocate(currUniqueKeyDesc);

        // setup violation output
        if (outAccessors.size() > 1) {
            violationAccessor = outAccessors[1];
            violationTuple.compute(violationAccessor->getTupleDesc());
        }
    }

    createNewIndex = params.createNewIndex;
    // If the index is going to be dynamically created, save the original
    // root pageId of the index so we can use it later to version the
    // index root page.
    if (createNewIndex) {
        origRootPageId = writeBTreeDesc.rootPageId;
    }
}

void LbmSplicerExecStream::open(bool restart)
{
    DiffluenceExecStream::open(restart);

    if (!restart) {
        bitmapBuffer.reset(new FixedBuffer[maxEntrySize]);
        mergeBuffer.reset(new FixedBuffer[maxEntrySize]);
        pCurrentEntry = SharedLbmEntry(new LbmEntry());
        pCurrentEntry->init(
            bitmapBuffer.get(), mergeBuffer.get(), maxEntrySize,
            bitmapTupleDesc);

        newIndexCreated = false;
        emptyTable = false;
        emptyTableUnknown = true;
        bTreeWriter = SharedBTreeWriter(
            new BTreeWriter(writeBTreeDesc, scratchAccessor, emptyTable));
        bTreeWriterMoved = true;

        if (opaqueToInt(writeRowCountParamId) > 0) {
            pDynamicParamManager->createParam(
                writeRowCountParamId,
                outAccessors[0]->getTupleDesc()[0]);
        }

        if (uniqueKey) {
            SharedBTreeReader deletionBTreeReader = SharedBTreeReader(
                new BTreeReader(deletionBTreeDesc));
            deletionReader.init(deletionBTreeReader, deletionTuple);
        }

        // If the index is going to be dynamically created, the underlying
        // segment associated with the index needs to be a snapshot segment.
        // Retrieve the snapshot segment.  This needs to be done at open time
        // because the segment changes across transaction boundaries.
        if (createNewIndex) {
            pSnapshotSegment =
                SegmentFactory::getSnapshotSegment(
                    writeBTreeDesc.segmentAccessor.pSegment);
            assert(pSnapshotSegment != NULL);
        }
    }
    isDone = false;
    currEntry = false;
    currExistingEntry = false;
    numRowsLoaded = 0;

    currValidation = false;
    firstValidation = true;
}

void LbmSplicerExecStream::getResourceRequirements(
    ExecStreamResourceQuantity &minQuantity,
    ExecStreamResourceQuantity &optQuantity)
{
    DiffluenceExecStream::getResourceRequirements(minQuantity, optQuantity);

    // btree pages
    minQuantity.nCachePages += 5;
    if (uniqueKey) {
        minQuantity.nCachePages += 5;
    }

    optQuantity = minQuantity;
}

bool LbmSplicerExecStream::isEmpty()
{
    if (emptyTableUnknown) {
        if (bTreeWriter->searchFirst() == false) {
            bTreeWriter->endSearch();
            emptyTable = true;
            // switch writer to monotonic now that we know the table
            // is empty
            bTreeWriter.reset();
            bTreeWriter = SharedBTreeWriter(
                new BTreeWriter(writeBTreeDesc, scratchAccessor, true));
        } else {
            emptyTable = false;
        }
        emptyTableUnknown = false;
    }
    return emptyTable;
}

ExecStreamResult LbmSplicerExecStream::execute(ExecStreamQuantum const &quantum)
{
    if (isDone) {
        // Version the index roots if the index was dynamically created
        if (newIndexCreated) {
            pSnapshotSegment->versionPage(
                origRootPageId,
                writeBTreeDesc.rootPageId);
        }

        for (uint i = 0; i < outAccessors.size(); i++) {
            outAccessors[i]->markEOS();
        }
        return EXECRC_EOS;
    }

    // no more input; write out last bitmap entry and produce final row count
    // which is either stored in a dynamic parameter set upstream or is
    // computed by splicer

    if (pInAccessor->getState() == EXECBUF_EOS) {
        if (currEntry) {
            insertBitmapEntry();
        }
        if (!computeRowCount) {
            numRowsLoaded = *reinterpret_cast<RecordNum const *>(
                pDynamicParamManager->getParam(
                    insertRowCountParamId).getDatum().pData);
        }
        if (opaqueToInt(writeRowCountParamId) > 0) {
            TupleDatum rowCountDatum;
            rowCountDatum.pData = (PConstBuffer) &numRowsLoaded;
            rowCountDatum.cbData = sizeof(numRowsLoaded);
            pDynamicParamManager->writeParam(
                writeRowCountParamId,
                rowCountDatum);
        }
        bool rc = outAccessors[0]->produceTuple(outputTuple);
        assert(rc);
        isDone = true;
        return EXECRC_BUF_OVERFLOW;
    }

    for (uint i = 0; i < quantum.nTuplesMax; i++) {
        ExecStreamResult rc = getValidatedTuple();
        if (rc != EXECRC_YIELD) {
            return rc;
        }

        if (uniqueRequired(inputTuple)) {
            if (currEntry) {
                // Write out the current entry before we insert the unique
                // key.
                insertBitmapEntry();
                currEntry = false;
            }
            upsertSingleton(inputTuple);
        } else if (!currEntry) {
            // if the key already exists in the index, splice the
            // entry just read to the existing btree entry
            if (existingEntry(inputTuple)) {
                spliceEntry(inputTuple);
            }
        } else {
            // Compare the key values of the currentEntry with the
            // input tuple.  If they're the same, try splicing with
            // currentEntry.  Otherwise, write out currentEntry and
            // set currentEntry to the new input
            int keyComp = pCurrentEntry->compareEntry(
                inputTuple, bitmapTupleDesc);
            assert(keyComp <= 0);
            if (keyComp == 0) {
                // If we're in the mode where we're splicing in random
                // singleton entries, first make sure there isn't a "better"
                // entry in the btree for the entry we're trying to splice in.
                // A better entry is one whose startRID is closer to the
                // singleton rid we're trying to splice in.
                if (computeRowCount) {
                    findBetterEntry(inputTuple);
                }
                spliceEntry(inputTuple);
            } else {
                insertBitmapEntry();
                if (existingEntry(inputTuple)) {
                    spliceEntry(inputTuple);
                }
            }
        }
        pInAccessor->consumeTuple();
    }

    return EXECRC_QUANTUM_EXPIRED;
}

void LbmSplicerExecStream::closeImpl()
{
    if (bTreeWriter) {
        bTreeWriter->endSearch();
    }
    deletionReader.endSearch();
    DiffluenceExecStream::closeImpl();
    bitmapBuffer.reset();
    mergeBuffer.reset();
    pCurrentEntry.reset();
    bTreeWriter.reset();
}

bool LbmSplicerExecStream::existingEntry(TupleData const &bitmapEntry)
{
    if (!isEmpty()) {
        // if entry already exists in the btree, then the current bitmap
        // entry becomes that existing btree entry
        if (findBTreeEntry(bitmapEntry, bTreeTupleData)) {
            currExistingEntry = true;
            createNewBitmapEntry(bTreeTupleData);
            bTreeWriterMoved = false;
            return true;
        }
    }

    // set current bitmap entry to new entry
    currExistingEntry = false;
    createNewBitmapEntry(bitmapEntry);
    return false;
}

bool LbmSplicerExecStream::findMatchingBTreeEntry(
    TupleData const &bitmapEntry,
    TupleData &bTreeTupleData,
    bool leastUpper)
{
    bool match =
        bTreeWriter->searchForKey(
            bitmapEntry,
            DUP_SEEK_BEGIN,
            leastUpper);
    bTreeWriter->getTupleAccessorForRead().unmarshal(bTreeTupleData);
    return match;
}

bool LbmSplicerExecStream::findBTreeEntry(
    TupleData const &bitmapEntry, TupleData &bTreeTupleData)
{
    // First do a greatest lower bound lookup into the btree, searching on
    // both the actual key index values and the startRid
    bool match =
        findMatchingBTreeEntry(bitmapEntry, bTreeTupleData, (nIdxKeys > 0));

    if (match == false) {
        if (nIdxKeys == 0) {
            // If there are no index keys, then we are splicing individual
            // rids.  In that case, we should always be splicing into the
            // best btree entry available.  First see if the greatest lower
            // bound entry overlaps the rid we're looking for.  If it doesn't,
            // try the next entry.  If that doesn't overlap, go back to the
            // greatest lower bound entry so we can splice the new rid to
            // the end of that entry.
            LcsRid newRid = *reinterpret_cast<LcsRid const *>
                (bitmapEntry[0].pData);
            if (!ridOverlaps(newRid, bTreeTupleData, false)) {
                match = bTreeWriter->searchNext();
                if (match) {
                    bTreeWriter->getTupleAccessorForRead().unmarshal(
                        bTreeTupleData);
                    if (!ridOverlaps(newRid, bTreeTupleData, true)) {
                        match = bTreeWriter->searchForKey(
                            bitmapEntry, DUP_SEEK_BEGIN, false);
                        assert(match == false);
                        bTreeWriter->getTupleAccessorForRead().unmarshal(
                            bTreeTupleData);
                    }
                }
            }
            match = true;

        } else {
            // In the case where we have actual index keys, we've done a
            // least upper bound search to locate the entry.  See if
            // the keys without the startRid match.  If they do, then we've
            // located a singleton rid that overlaps with the entry we're
            // trying to splice.  If so, that is the entry we want to splice
            // into.  Otherwise, the desired entry may be in front of the
            // one we've located.  Therefore, we need to do a greatest lower
            // bound search to locate that previous entry (since we don't have
            // a BTreeReader::searchPrev method), and then compare the keys
            // to see if we have a match.
            if (!bTreeWriter->isSingular()) {
                int keyComp =
                    bitmapTupleDesc.compareTuplesKey(
                        bTreeTupleData,
                        bitmapEntry,
                        nIdxKeys);
                if (keyComp == 0) {
                    assert(
                        LbmSegment::roundToByteBoundary(
                            *reinterpret_cast<LcsRid const *>(
                                bTreeTupleData[nIdxKeys].pData))
                        == LbmSegment::roundToByteBoundary(
                            *reinterpret_cast<LcsRid const *>(
                                bitmapEntry[nIdxKeys].pData)));
                    return true;
                }
            }

            // Position to the previous entry by doing a glb search
            match =
                bTreeWriter->searchForKey(bitmapEntry, DUP_SEEK_BEGIN, false);
            assert(match == false);
            bTreeWriter->getTupleAccessorForRead().unmarshal(bTreeTupleData);
            int keyComp =
                bitmapTupleDesc.compareTuplesKey(
                    bTreeTupleData,
                    bitmapEntry,
                    nIdxKeys);
            if (keyComp == 0) {
                match = true;
            }
        }
    }
    return match;
}

bool LbmSplicerExecStream::ridOverlaps(
    LcsRid rid,
    TupleData &bitmapTupleData,
    bool firstByte)
{
    // Convert singletons to the rid range representing all bits in the byte
    // corresponding to the singleton rid
    LcsRid startRid =
        LbmSegment::roundToByteBoundary(
            *reinterpret_cast<LcsRid const *>(bitmapTupleData[0].pData));
    uint rowCount;
    if (firstByte) {
       rowCount = LbmSegment::LbmOneByteSize;
    } else {
        rowCount = LbmEntry::getRowCount(bitmapTupleData);
        if (rowCount == 1) {
            rowCount = LbmSegment::LbmOneByteSize;
        }
    }
    if (rid >= startRid && rid < startRid + rowCount) {
        return true;
    } else {
        return false;
    }
}

void LbmSplicerExecStream::findBetterEntry(TupleData const &bitmapEntry)
{
    // If there is a better btree entry, write out the current entry and set
    // the current entry to the btree entry found.  The btree entry is "better"
    // if it's the entry that we should be splicing the new rid into.
    //
    // In other words, one of the following conditions must be true:
    //
    // 1) bTreeStartRid <= newRid < currentStartRid
    // 2) currentStartRid < bTreeStartRid <= newRid
    // 3) newRid <= bTreeStartRid < currentStartRid
    //
    // NOTE - condition 1 occurs when the current bitmap entry is split, and
    // the current entry becomes the right portion of that bitmap entry.
    // Also, conditions 1 and 3 can be combined into:
    //
    // currentStartRid > newRid && currentStartRid > bTreeStartRid

    assert(computeRowCount);
    if (!isEmpty()) {
        if (findBTreeEntry(bitmapEntry, bTreeTupleData)) {
            LcsRid bTreeRid =
                LbmSegment::roundToByteBoundary(
                    *reinterpret_cast<LcsRid const *> (
                        bTreeTupleData[0].pData));
            LcsRid newRid = *reinterpret_cast<LcsRid const *>
                (bitmapEntry[0].pData);
            LcsRid currRid =
                LbmSegment::roundToByteBoundary(pCurrentEntry->getStartRID());

            if ((currRid > newRid && currRid > bTreeRid)
                || (newRid >= bTreeRid && bTreeRid > currRid))
            {
                // If the current entry is a superset of the btree entry found,
                // then ignore the btree entry, and continuing splicing into
                // the current entry
                uint rowCount = pCurrentEntry->getRowCount();
                if (rowCount == 1) {
                    rowCount = LbmSegment::LbmOneByteSize;
                }
                if ((bTreeRid >= currRid) && (bTreeRid < currRid + rowCount)) {
                    return;
                }

                // Write out the current entry before we switch over to the
                // new one
                insertBitmapEntry();
                currExistingEntry = true;
                createNewBitmapEntry(bTreeTupleData);
            }
        }
    }
}

void LbmSplicerExecStream::spliceEntry(TupleData &bitmapEntry)
{
    FENNEL_TRACE(TRACE_FINE, "splice two entries");
    FENNEL_TRACE(TRACE_FINE, pCurrentEntry->toString());
    FENNEL_TRACE(TRACE_FINE, LbmEntry::toString(bitmapEntry));

    if (!pCurrentEntry->mergeEntry(bitmapEntry)) {
        insertBitmapEntry();
        createNewBitmapEntry(bitmapEntry);
    }
}

void LbmSplicerExecStream::insertBitmapEntry()
{
    TupleData const &indexTuple = pCurrentEntry->produceEntryTuple();

    // implement btree updates as deletes followed by inserts
    if (currExistingEntry) {
        // when we're inserting in random singleton mode, we may have
        // repositioned in the btree, trying to find a better btree entry,
        // so we need to position back to the original btree entry before
        // we delete it. the btree may also reposition for validation
        if (bTreeWriterMoved) {
            for (uint i = 0; i < nIdxKeys; i++) {
                tempBTreeTupleData[i] = indexTuple[i];
            }
        }
        if (computeRowCount || bTreeWriterMoved) {
            tempBTreeTupleData[nIdxKeys].pData =
                (PConstBuffer) &currBTreeStartRid;
            bool match =
                findMatchingBTreeEntry(
                    tempBTreeTupleData,
                    tempBTreeTupleData,
                    false);
            permAssert(match);
        }
        FENNEL_TRACE(TRACE_FINE, "delete Tuple from BTree");
        FENNEL_TRACE(TRACE_FINE, LbmEntry::toString(bTreeTupleData));

        bTreeWriter->deleteCurrent();
        currExistingEntry = false;
    }

    FENNEL_TRACE(TRACE_FINE, "insert Tuple into BTree");
    FENNEL_TRACE(TRACE_FINE, LbmEntry::toString(indexTuple));

    bTreeWriter->insertTupleData(indexTuple, DUP_FAIL);
}

void LbmSplicerExecStream::createNewBitmapEntry(TupleData const &bitmapEntry)
{
    pCurrentEntry->setEntryTuple(bitmapEntry);
    currBTreeStartRid = *reinterpret_cast<LcsRid const *>
        (bitmapEntry[nIdxKeys].pData);
    currEntry = true;
}

void LbmSplicerExecStream::upsertSingleton(TupleData const &bitmapEntry)
{
    if (!isEmpty()) {
        if (findBTreeEntry(bitmapEntry, bTreeTupleData)) {
            assert(LbmEntry::isSingleton(bTreeTupleData));
            bTreeWriter->deleteCurrent();
        }
    }
    bTreeWriter->insertTupleData(bitmapEntry, DUP_FAIL);
}

ExecStreamResult LbmSplicerExecStream::getValidatedTuple()
{
    while (true) {
        if (!currValidation) {
            if (!pInAccessor->demandData()) {
                return EXECRC_BUF_UNDERFLOW;
            }

            if (computeRowCount) {
                pInAccessor->unmarshalTuple(singletonTuple);
                inputTuple[0] = singletonTuple[0];
                inputTuple[1].pData = NULL;
                inputTuple[1].cbData = 0;
                inputTuple[2].pData = NULL;
                inputTuple[2].cbData = 0;
                numRowsLoaded++;
            } else {
                pInAccessor->unmarshalTuple(inputTuple);
            }

            FENNEL_TRACE(TRACE_FINE, "input Tuple from sorter");
            FENNEL_TRACE(TRACE_FINE, LbmEntry::toString(inputTuple));

            // If we're creating a new index, we need to defer creating it until
            // we know that there are new input tuples.  Otherwise, there's no
            // point in creating it.
            if (createNewIndex && !newIndexCreated) {
                newIndexCreated = true;
                writeBTreeDesc.rootPageId = NULL_PAGE_ID;
                BTreeBuilder builder(
                    writeBTreeDesc,
                    writeBTreeDesc.segmentAccessor.pSegment);
                builder.createEmptyRoot();
                writeBTreeDesc.rootPageId = builder.getRootPageId();
                emptyTable = true;
                emptyTableUnknown = false;
                bTreeWriter = SharedBTreeWriter(
                    new BTreeWriter(
                        writeBTreeDesc,
                        scratchAccessor,
                        emptyTable));
            }

            if (!uniqueRequired(inputTuple)) {
                return EXECRC_YIELD;
            }

            // count existing entries for key, if the key has not been seen yet
            if (firstValidation
                || bitmapTupleDesc.compareTuplesKey(
                    inputTuple, currUniqueKey, nIdxKeys) != 0)
            {
                firstValidation = false;
                currUniqueKey.resetBuffer();
                for (uint i = 0; i < nIdxKeys; i++) {
                    currUniqueKey[i].memCopyFrom(inputTuple[i]);
                }
                nKeyRows = countKeyRows(inputTuple);
            }

            // prepare to emit rids for key violations
            inputRidReader.init(inputTuple);
            nullUpsertRid = true;
            currValidation = true;
        }

        // if there were no undeleted values for the current key, we can
        // insert/update a single rid
        if (nKeyRows == 0) {
            assert(getUpsertRidPtr() == NULL);
            if (!createNewIndex) {
                setUpsertRid(inputRidReader.getNext());
                nKeyRows++;
            } else {
                // Loop until we find a non-deleted rid.  Deleted rids only
                // occur when rebuilding an existing index.
                do {
                    LcsRid rid = inputRidReader.getNext();
                    if (!deletionReader.searchForRid(rid)) {
                        setUpsertRid(rid);
                        nKeyRows++;
                        break;
                    }
                } while (inputRidReader.hasNext());
            }
        }

        // all other rids are rejected as duplicate keys, unless they're deleted
        // rids
        while (inputRidReader.hasNext()) {
            if (!violationTuple.size()) {
                // if there is a possibility of violations, the splicer should
                // have been initialized with a second output
                permAssert(false);
            }
            LcsRid rid = inputRidReader.peek();
            if (createNewIndex && deletionReader.searchForRid(rid)) {
                inputRidReader.advance();
                continue;
            }
            violationTuple[0].pData = reinterpret_cast<PConstBuffer>(&rid);
            violationTuple[0].cbData = 8;
            if (!violationAccessor->produceTuple(violationTuple)) {
                return EXECRC_BUF_OVERFLOW;
            }
            postViolation(inputTuple, violationTuple);
            inputRidReader.advance();
        }
        currValidation = false;

        if (getUpsertRidPtr() != NULL) {
            // since a rid was accepted, return it as a validated tuple
            inputTuple[nIdxKeys].pData =
                reinterpret_cast<PConstBuffer>(getUpsertRidPtr());
            inputTuple[nIdxKeys].cbData = 8;
            inputTuple[nIdxKeys + 1].pData = NULL;
            inputTuple[nIdxKeys + 1].cbData = 0;
            inputTuple[nIdxKeys + 2].pData = NULL;
            inputTuple[nIdxKeys + 2].cbData = 0;
            return EXECRC_YIELD;
        }

        // every rid in the current tuple was either rejected or already
        // deleted, so try the next tuple
        pInAccessor->consumeTuple();
    }
}

bool LbmSplicerExecStream::uniqueRequired(const TupleData &tuple)
{
    if (uniqueKey) {
        for (uint i = 0; i < nIdxKeys; i++) {
            if (tuple[i].isNull()) {
                return false;
            }
        }
        return true;
    }
    return false;
}

uint LbmSplicerExecStream::countKeyRows(const TupleData &tuple)
{
    assert(uniqueKey);
    if (isEmpty()) {
        return 0;
    }

    if (!findBTreeEntry(inputTuple, bTreeTupleData)) {
        return 0;
    }
    assert(LbmEntry::isSingleton(bTreeTupleData));
    LcsRid rid = LbmEntry::getStartRid(bTreeTupleData);

    if (deletionReader.searchForRid(rid)) {
        return 0;
    }
    return 1;
}

void LbmSplicerExecStream::postViolation(
    const TupleData &input, const TupleData &violation)
{
    if (!errorTuple.size()) {
        for (uint i = 0; i < nIdxKeys + 1; i++) {
            errorDesc.push_back(bitmapTupleDesc[i]);
        }
        errorTuple.compute(errorDesc);
        errorMsg = FennelResource::instance().uniqueConstraintViolated();
    }

    for (uint i = 0; i < nIdxKeys; i++) {
        errorTuple[i] = input[i];
    }
    errorTuple[nIdxKeys] = violation[0];

    postError(ROW_ERROR, errorMsg, errorDesc, errorTuple, -1);
}

FENNEL_END_CPPFILE("$Id$");

// End LbmSplicerExecStream.cpp
