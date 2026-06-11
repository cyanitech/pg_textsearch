/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * scan.c - BM25 index scan operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/heapam.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/rel.h>

#include "access/am.h"
#include "constants.h"
#include "index/limit.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/state.h"
#include "memtable/chain_walker.h"
#include "memtable/page.h"
#include "memtable/scan.h"
#include "types/query.h"
#include "types/vector.h"

/*
 * Backend-local cached score for ORDER BY optimization.
 *
 * When tp_gettuple returns a row, the BM25 score is cached here. The
 * bm25_get_current_score() stub function returns this value, avoiding
 * re-computation of scores in resjunk ORDER BY expressions.
 */
static float8 tp_cached_score = 0.0;

float8
tp_get_cached_score(void)
{
	return tp_cached_score;
}

/*
 * Clean up any previous scan results in the scan opaque structure
 */
static void
tp_rescan_cleanup_results(TpScanOpaque so)
{
	if (!so)
		return;

	Assert(so->scan_context != NULL);

	/* Clean up result CTIDs */
	if (so->result_ctids)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
		pfree(so->result_ctids);
		so->result_ctids = NULL;
		MemoryContextSwitchTo(oldcontext);
	}

	/* Clean up result scores */
	if (so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
		pfree(so->result_scores);
		so->result_scores = NULL;
		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Process ORDER BY scan keys for <@> operator
 *
 * Handles both bm25query and plain text arguments to support:
 * - ORDER BY content <@> 'query'::bm25query (explicit bm25query)
 * - ORDER BY content <@> 'query' (plain text, implicit index resolution)
 */
static void
tp_rescan_process_orderby(
		IndexScanDesc	scan,
		ScanKey			orderbys,
		int				norderbys,
		TpIndexMetaPage metap)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	for (int i = 0; i < norderbys; i++)
	{
		ScanKey orderby = &orderbys[i];

		/* Check for <@> operator strategy */
		if (orderby->sk_strategy == 1) /* Strategy 1: <@> operator */
		{
			Datum query_datum = orderby->sk_argument;
			char *query_cstr;
			Oid	  query_index_oid = InvalidOid;

			/*
			 * Use sk_subtype to determine the argument type.
			 * sk_subtype contains the right-hand operand's type OID.
			 */
			if (orderby->sk_subtype == TEXTOID)
			{
				/* Plain text - use text directly */
				text *query_text = (text *)DatumGetPointer(query_datum);

				query_cstr = text_to_cstring(query_text);
			}
			else
			{
				/* bm25query - extract query text and index OID */
				TpQuery *query = (TpQuery *)DatumGetPointer(query_datum);

				query_cstr		= pstrdup(get_tpquery_text(query));
				query_index_oid = get_tpquery_index_oid(query);

				/* Validate index OID if provided in query */
				if (tpquery_has_index(query))
				{
					tp_validate_query_index(
							query_index_oid, scan->indexRelation);
				}

				so->phrase_mode = tpquery_is_phrase(query);
			}

			/* Clear query vector since we're using text directly */
			if (so->query_vector)
			{
				pfree(so->query_vector);
				so->query_vector = NULL;
			}

			/* Free old query text if it exists */
			if (so->query_text)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				pfree(so->query_text);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Allocate new query text in scan context */
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				so->query_text = pstrdup(query_cstr);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Store index OID for this scan */
			so->index_oid = RelationGetRelid(scan->indexRelation);

			/* Mark all docs as candidates for ORDER BY operation */
			if (metap && metap->total_docs > 0)
				so->result_count = metap->total_docs;

			pfree(query_cstr);
		}
	}
}

/*
 * Begin a scan of the Tapir index
 */
IndexScanDesc
tp_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	TpScanOpaque  so;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Allocate and initialize scan opaque data */
	so				 = (TpScanOpaque)palloc0(sizeof(TpScanOpaqueData));
	so->scan_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Tapir Scan Context",
			ALLOCSET_DEFAULT_SIZES);
	so->limit			 = -1; /* Initialize limit to -1 (no limit) */
	so->max_results_used = 0;
	scan->opaque		 = so;

	/*
	 * Custom index AMs must allocate ORDER BY arrays themselves.
	 */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals  = (Datum *)palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
		/* Initialize all orderbynulls to true */
		memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
	}

	return scan;
}

/*
 * Restart a scan with new keys
 */
void
tp_rescan(
		IndexScanDesc scan,
		ScanKey		  keys __attribute__((unused)),
		int			  nkeys __attribute__((unused)),
		ScanKey		  orderbys,
		int			  norderbys)
{
	TpScanOpaque	so	  = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap = NULL;

	Assert(scan != NULL);
	Assert(scan->opaque != NULL);

	if (!so)
		return;

	/* Retrieve query LIMIT, if available */
	{
		int query_limit = tp_get_query_limit(scan->indexRelation);
		so->limit		= (query_limit > 0) ? query_limit : -1;
	}

	/* Reset scan state */
	if (so)
	{
		/* Clean up any previous results */
		tp_rescan_cleanup_results(so);

		/* Reset scan position and state */
		so->current_pos	 = 0;
		so->result_count = 0;
		so->raw_result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;
		so->phrase_mode = false;
	}

	/* Process ORDER BY scan keys for <@> operator */
	if (norderbys > 0 && orderbys && so)
	{
		/* Get index metadata to check if we have documents */
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		tp_rescan_process_orderby(scan, orderbys, norderbys, metap);

		if (metap)
			pfree(metap);
	}
}

/*
 * Verify phrase match for a single candidate CTID by first attempting
 * to read the TpVector (with positions) from the on-disk memtable
 * chain, then falling back to heap_fetch + re-tokenize.
 */
static bool
tp_verify_single_phrase_ctid(
		IndexScanDesc scan, ItemPointer ctid,
		TpVector *tpvec, Oid text_config_oid)
{
	if (tpvec != NULL && tpvector_has_positions(tpvec))
		return tp_verify_phrase_with_tpvector(
				tpvec,
				((TpScanOpaque)scan->opaque)->query_text,
				text_config_oid);

	/* Fallback: heap recheck for CTIDs not in chain */
	return false; /* caller will try heap path */
}

/*
 * Walk the memtable chain and build a CTID→TpVector lookup for
 * phrase verification.  Only stores records whose CTID matches one
 * of the candidates, and only when positions are present.
 *
 * Returns a hash table (CTID→TpVector) or NULL if the chain is
 * empty or has no position-bearing records.
 */
static HTAB *
tp_build_ctid_vector_hash(IndexScanDesc scan, int raw_result_count)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	TpChainWalker	  *walker;
	TpChainWalkerRecord rec;
	HTAB			  *ctid_ht = NULL;
	HASHCTL			   ctl;
	TpIndexMetaPage	   metap;
	MemoryContext	   oldctx;

	metap = tp_get_metapage(scan->indexRelation);
	if (metap == NULL || metap->memtable_head_blkno == InvalidBlockNumber)
	{
		if (metap) pfree(metap);
		return NULL;
	}

	/* Build hash table keyed by raw ItemPointerData */
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize   = sizeof(ItemPointerData);
	ctl.entrysize = sizeof(ItemPointerData) + sizeof(TpVector *);
	ctl.hcxt	   = CurrentMemoryContext;
	ctid_ht = hash_create("bm25 phrase ctid→vector",
			raw_result_count, &ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Pre-populate: all candidates flagged as "not yet found" */
	for (int i = 0; i < raw_result_count; i++)
	{
		bool found;
		TpVector **slot = (TpVector **)hash_search(
				ctid_ht, &so->result_ctids[i], HASH_ENTER, &found);

		Assert(!found);
		if (slot)
			*slot = NULL;
	}

	/*
	 * Walk the chain.  For each record whose CTID is in our hash
	 * table, store a reference to the TpVector bytes.  We only care
	 * about records that carry positions.
	 */
	walker = tp_chain_walker_open(
			scan->indexRelation,
			metap->memtable_head_blkno,
			0,
			CurrentMemoryContext);

	pfree(metap);

	while (tp_chain_walker_next(walker, &rec))
	{
		bool		found;
		TpVector   **slot;
		TpVector	*vec;

		slot = (TpVector **)hash_search(
				ctid_ht, &rec.ctid, HASH_FIND, &found);
		if (!found || slot == NULL)
			continue;

		/* Already filled by a duplicate? Skip. */
		if (*slot != NULL)
			continue;

		vec = (TpVector *)rec.vector_bytes;
		if (!tpvector_has_positions(vec))
			continue;

		/*
		 * For fragments, the vector is palloc'd and survives the
		 * walker; for inline records it's in a buffer page and must
		 * be copied.
		 */
		if (rec.is_fragment)
		{
			*slot = vec; /* survives walker close */
		}
		else
		{
			oldctx = MemoryContextSwitchTo(CurrentMemoryContext);
			*slot = (TpVector *)palloc(rec.vector_len);
			memcpy(*slot, rec.vector_bytes, rec.vector_len);
			MemoryContextSwitchTo(oldctx);
		}
	}

	tp_chain_walker_close(walker);
	return ctid_ht;
}

static bool
tp_filter_phrase_results(
		IndexScanDesc scan, Oid text_config_oid, int raw_result_count)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	Relation	 heap_rel;
	AttrNumber	 attnum;
	TupleDesc	 heap_desc;
	HeapTupleData tuple_data;
	HeapTuple	 tuple = &tuple_data;
	Buffer		 heap_buf = InvalidBuffer;
	ItemPointerData *filtered_ctids;
	float4		   *filtered_scores;
	int				 filtered_count = 0;
	HTAB		   *ctid_vector_ht = NULL;

	if (!so->phrase_mode || raw_result_count <= 0)
		return raw_result_count > 0;

	if (scan->indexRelation->rd_index == NULL ||
		scan->indexRelation->rd_index->indnatts < 1 ||
		scan->indexRelation->rd_index->indkey.values[0] <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("phrase queries are not yet supported on BM25 "
						"expression indexes")));

	attnum = scan->indexRelation->rd_index->indkey.values[0];
	heap_rel = table_open(
			scan->indexRelation->rd_index->indrelid, AccessShareLock);
	heap_desc = RelationGetDescr(heap_rel);

	if (TupleDescAttr(heap_desc, attnum - 1)->atttypid != TEXTOID)
	{
		table_close(heap_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("phrase queries currently support only text columns")));
	}

	/*
	 * Build CTID→TpVector hash from the memtable chain so we can
	 * verify phrase matches directly from stored positions.
	 */
	ctid_vector_ht = tp_build_ctid_vector_hash(scan, raw_result_count);

	filtered_ctids	= palloc(raw_result_count * sizeof(ItemPointerData));
	filtered_scores = palloc(raw_result_count * sizeof(float4));

	for (int i = 0; i < raw_result_count; i++)
	{
		bool	matched	   = false;
		bool	tried_chain = false;

		/*
		 * Primary path: check stored positions from the memtable
		 * chain.  This is the fast, zero-heap-access path.
		 */
		if (ctid_vector_ht != NULL)
		{
			bool		found;
			TpVector   **slot;

			slot = (TpVector **)hash_search(
					ctid_vector_ht, &so->result_ctids[i],
					HASH_FIND, &found);

			if (found && slot != NULL && *slot != NULL)
			{
				tried_chain = true;
				matched = tp_verify_single_phrase_ctid(
						scan, &so->result_ctids[i],
						*slot, text_config_oid);
			}
		}

		/*
		 * Fallback: heap recheck for CTIDs not found in the
		 * chain (e.g. rows that only exist in segments).
		 */
		if (!tried_chain)
		{
			Datum document_datum;
			bool  isnull;
			bool  valid;

			tuple->t_self = so->result_ctids[i];
			valid = heap_fetch(heap_rel, SnapshotAny,
					tuple, &heap_buf, true);
			if (!valid)
			{
				if (heap_buf != InvalidBuffer)
					ReleaseBuffer(heap_buf);
				heap_buf = InvalidBuffer;
				continue;
			}

			document_datum = heap_getattr(
					tuple, attnum, heap_desc, &isnull);
			if (!isnull)
				matched = tp_phrase_match_document_text(
						DatumGetTextPP(document_datum),
						so->query_text,
						text_config_oid);

			if (heap_buf != InvalidBuffer)
				ReleaseBuffer(heap_buf);
			heap_buf = InvalidBuffer;
		}

		if (matched)
		{
			filtered_ctids[filtered_count] = so->result_ctids[i];
			filtered_scores[filtered_count] = so->result_scores[i];
			filtered_count++;
		}
	}

	table_close(heap_rel, AccessShareLock);

	if (ctid_vector_ht != NULL)
		hash_destroy(ctid_vector_ht);

	pfree(so->result_ctids);
	pfree(so->result_scores);
	so->result_ctids  = filtered_ctids;
	so->result_scores = filtered_scores;
	so->result_count  = filtered_count;
	so->current_pos   = 0;

	return filtered_count > 0;
}
/*
 * End a scan and cleanup resources
 */
void
tp_endscan(IndexScanDesc scan)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	if (so)
	{
		if (so->scan_context)
			MemoryContextDelete(so->scan_context);

		/* Free query vector if it was allocated */
		if (so->query_vector)
			pfree(so->query_vector);

		pfree(so);
		scan->opaque = NULL;
	}

	/*
	 * Don't free ORDER BY arrays here - PostgreSQL's core code will free them.
	 */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals  = NULL;
		scan->xs_orderbynulls = NULL;
	}
}

/*
 * Execute BM25 scoring query to get ordered results
 */
static bool
tp_execute_scoring_query(IndexScanDesc scan)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage	   metap;
	bool			   success	   = false;
	TpLocalIndexState *index_state = NULL;
	TpVector		  *query_vector;

	if (!so || !so->query_text)
		return false;

	Assert(so->scan_context != NULL);

	/* Clean up previous results */
	if (so->result_ctids || so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);

		if (so->result_ctids)
		{
			pfree(so->result_ctids);
			so->result_ctids = NULL;
		}
		if (so->result_scores)
		{
			pfree(so->result_scores);
			so->result_scores = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}

	so->result_count = 0;
	so->raw_result_count = 0;
	so->current_pos	 = 0;

	/* Get the index state with posting lists */
	index_state = tp_get_local_index_state(
			RelationGetRelid(scan->indexRelation));

	if (!index_state)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for BM25 "
						"search")));
	}

	/*
		so->raw_result_count = result_count;
		so->result_count	 = result_count;
	 * This ensures the metapage and memtable are read in a
	 * consistent state — spill (which rewrites both) requires

		if (result_count > 0 && so->phrase_mode)
		{
			(void)tp_filter_phrase_results(
					scan, metap->text_config_oid, result_count);
			success = (so->raw_result_count > 0);
		}
		else
			success = (result_count > 0);
	 * LW_EXCLUSIVE, which is blocked while we hold shared.
	 */
	tp_acquire_index_lock(index_state, LW_SHARED);

	/* Now read metapage under the lock */
	metap = tp_get_metapage(scan->indexRelation);
	if (!metap)
	{
		return success;
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to get metapage for index %s",
						RelationGetRelationName(scan->indexRelation))));
	}

	/* Use the original query vector or create one from text */
	query_vector = so->query_vector;

	if (!query_vector && so->query_text)
	{
		/*
		 * We have a text query - convert it to a vector using the index.
		 */
		char *index_name = tp_get_qualified_index_name(scan->indexRelation);

		text *index_name_text  = cstring_to_text(index_name);
		text *query_text_datum = cstring_to_text(so->query_text);

		Datum query_vec_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(query_text_datum),
				PointerGetDatum(index_name_text));

		query_vector = (TpVector *)DatumGetPointer(query_vec_datum);

		/* Free existing query vector if present */
		if (so->query_vector)
			pfree(so->query_vector);

		/* Store the converted vector for this query execution */
		so->query_vector = query_vector;
	}

	if (!query_vector)
	{
		pfree(metap);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("no query vector available in scan state")));
	}

	/* Find documents matching the query using posting lists */
	success = tp_memtable_search(scan, index_state, query_vector, metap);

	/* Release the lock - we've extracted all CTIDs we need */
	tp_release_index_lock(index_state);

	pfree(metap);
	return success;
}

/*
 * Get next tuple from scan
 */
bool
tp_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	float4		 bm25_score;
	BlockNumber	 blknum;

	(void)dir; /* BM25 index only supports forward scan */

	Assert(scan != NULL);
	Assert(so != NULL);
	Assert(so->query_text != NULL);

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && !so->eof_reached)
	{
		/* Count index scan for pg_stat_user_indexes */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		if (!tp_execute_scoring_query(scan))
		{
			so->eof_reached = true;
			return false;
		}
		/* Scoring query must have allocated result_ctids on success */
		if (so->result_ctids == NULL)
		{
			so->eof_reached = true;
			return false;
		}
	}

	/* Check if we've reached the end of current result set */
	if (so->current_pos >= so->result_count || so->eof_reached)
	{
		/*
		 * If result_count hit the internal limit, there may be more
		 * documents.  Double the limit and re-execute the scoring
		 * query, skipping already-returned results.
		 */
		if (!so->eof_reached && so->result_count > 0 &&
			so->raw_result_count >= so->max_results_used &&
			so->max_results_used < TP_MAX_QUERY_LIMIT)
		{
			int old_count = so->result_count;
			int new_limit = so->max_results_used * 2;

			if (new_limit > TP_MAX_QUERY_LIMIT)
				new_limit = TP_MAX_QUERY_LIMIT;

			so->limit = new_limit;
			if (tp_execute_scoring_query(scan) && so->result_count > old_count)
			{
				/* Skip already-returned results */
				so->current_pos = old_count;
				/* Fall through to return next tuple */
			}
			else
			{
				so->eof_reached = true;
				return false;
			}
		}
		else
			return false;
	}

	Assert(so->scan_context != NULL);
	Assert(so->result_ctids != NULL);
	Assert(so->current_pos < so->result_count);

	Assert(ItemPointerIsValid(&so->result_ctids[so->current_pos]));

	/* Skip results with invalid block numbers */
	blknum = BlockIdGetBlockNumber(
			&(so->result_ctids[so->current_pos].ip_blkid));
	while (blknum == InvalidBlockNumber)
	{
		so->current_pos++;
		if (so->current_pos >= so->result_count)
			return false;
		blknum = BlockIdGetBlockNumber(
				&(so->result_ctids[so->current_pos].ip_blkid));
	}

	scan->xs_heaptid		= so->result_ctids[so->current_pos];
	scan->xs_recheck		= false;
	scan->xs_recheckorderby = false;

	/* Set ORDER BY distance value */
	if (scan->numberOfOrderBys > 0)
	{
		float4 raw_score;

		Assert(scan->numberOfOrderBys == 1);
		Assert(scan->xs_orderbyvals != NULL);
		Assert(scan->xs_orderbynulls != NULL);
		Assert(so->result_scores != NULL);

		/* Convert BM25 score to Datum (ensure negative for ASC sort) */
		raw_score				 = so->result_scores[so->current_pos];
		bm25_score				 = (raw_score > 0) ? -raw_score : raw_score;
		scan->xs_orderbyvals[0]	 = Float4GetDatum(bm25_score);
		scan->xs_orderbynulls[0] = false;

		/* Log BM25 score if enabled */
		elog(tp_log_scores ? NOTICE : DEBUG1,
			 "BM25 index scan: tid=(%u,%u), BM25_score=%.4f",
			 BlockIdGetBlockNumber(&scan->xs_heaptid.ip_blkid),
			 scan->xs_heaptid.ip_posid,
			 bm25_score);

		/* Cache score for stub function to retrieve */
		tp_cached_score = (float8)bm25_score;
	}

	/* Move to next position */
	so->current_pos++;

	return true;
}
