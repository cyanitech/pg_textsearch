-- Phrase search (==) regression tests
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET client_min_messages = WARNING;

-- =====================================================
-- Section 1: Basic two-word phrase with simple config
-- =====================================================
CREATE TABLE phrase_basic (id int, content text);
CREATE INDEX phrase_basic_idx ON phrase_basic USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_basic VALUES
    (1, 'hello world this is a test'),
    (2, 'world hello is different order'),
    (3, 'hello to the world of databases'),
    (4, 'hello world again here');

-- == should match docs where "hello world" appear adjacently
SELECT id FROM phrase_basic
WHERE content == 'hello world'
ORDER BY id;

-- Negative: reversed order should NOT match
SELECT id FROM phrase_basic
WHERE content == 'world hello'
ORDER BY id;

-- Negative: words separated by another token
SELECT id FROM phrase_basic
WHERE content == 'hello test'
ORDER BY id;

-- == with ORDER BY score + LIMIT
SELECT id, bm25_get_current_score() AS score FROM phrase_basic
WHERE content == 'hello world'
ORDER BY score
LIMIT 3;

DROP TABLE phrase_basic;

-- =====================================================
-- Section 2: Three-word phrase
-- =====================================================
CREATE TABLE phrase_three (id int, content text);
CREATE INDEX phrase_three_idx ON phrase_three USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_three VALUES
    (1, 'the quick brown fox jumps'),
    (2, 'quick brown fox was here'),
    (3, 'fox brown quick is wrong order'),
    (4, 'quick brown lazy fox has gap');

SELECT id FROM phrase_three
WHERE content == 'quick brown fox'
ORDER BY id;

-- Negative: wrong order
SELECT id FROM phrase_three
WHERE content == 'fox brown quick'
ORDER BY id;

DROP TABLE phrase_three;

-- =====================================================
-- Section 3: Repeated words in phrase
-- =====================================================
CREATE TABLE phrase_repeat (id int, content text);
CREATE INDEX phrase_repeat_idx ON phrase_repeat USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_repeat VALUES
    (1, 'one one one'),
    (2, 'one one two'),
    (3, 'one two one'),
    (4, 'two one one'),
    (5, 'one one one two');

-- Phrase with repeated lexeme
SELECT id FROM phrase_repeat
WHERE content == 'one one'
ORDER BY id;

-- Three repeated
SELECT id FROM phrase_repeat
WHERE content == 'one one one'
ORDER BY id;

DROP TABLE phrase_repeat;

-- =====================================================
-- Section 4: Stop-word gap with english config
-- =====================================================
CREATE TABLE phrase_stop (id int, content text);
CREATE INDEX phrase_stop_idx ON phrase_stop USING bm25(content)
    WITH (text_config='english');

-- "bank of china" → stop-word "of" is removed, positions: bank:1, china:3
-- So doc must have pos(china) = pos(bank) + 2
INSERT INTO phrase_stop VALUES
    (1, 'Bank of China tower'),
    (2, 'bank and china separated'),       -- gap is 2, not 1
    (3, 'Bank of America and China'),      -- gap too large
    (4, 'china bank of wrong order');

-- "bank of china" should match only doc 1 (bank:1, china:3, gap=2)
SELECT id FROM phrase_stop
WHERE content == 'bank of china'
ORDER BY id;

-- Non-stop-word three-word phrase
SELECT id FROM phrase_stop
WHERE content == 'china tower'
ORDER BY id;

-- Negative: "bank china" without stop-word should NOT match doc 1
-- (because positions are bank:1, china:3 → gap is 2, not 1)
SELECT id FROM phrase_stop
WHERE content == 'bank china'
ORDER BY id;

DROP TABLE phrase_stop;

-- =====================================================
-- Section 5: English stemming
-- =====================================================
CREATE TABLE phrase_stem (id int, content text);
CREATE INDEX phrase_stem_idx ON phrase_stem USING bm25(content)
    WITH (text_config='english');

-- Stemming normalizes "running"→"run", "dogs"→"dog" etc.
INSERT INTO phrase_stem VALUES
    (1, 'the running dogs are here'),
    (2, 'run dog is simple'),
    (3, 'dogs are running fast');

-- Query with stemmed forms: lemmatized to the same lexemes
SELECT id FROM phrase_stem
WHERE content == 'running dogs'
ORDER BY id;

DROP TABLE phrase_stem;

-- =====================================================
-- Section 6: Empty / single-word / no-match
-- =====================================================
CREATE TABLE phrase_edge (id int, content text);
CREATE INDEX phrase_edge_idx ON phrase_edge USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_edge VALUES
    (1, 'hello'),
    (2, 'world only'),
    (3, 'hello world');

-- Single word == should work (trivially a phrase of length 1)
SELECT id FROM phrase_edge
WHERE content == 'hello'
ORDER BY id;

-- No match
SELECT id FROM phrase_edge
WHERE content == 'nonexistent'
ORDER BY id;

-- Empty string (should return no rows)
SELECT id FROM phrase_edge
WHERE content == ''
ORDER BY id;

DROP TABLE phrase_edge;

-- =====================================================
-- Section 7: ORDER BY score with BM25 ranking
-- =====================================================
CREATE TABLE phrase_rank (id int, content text);
CREATE INDEX phrase_rank_idx ON phrase_rank USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_rank VALUES
    (1, 'database database database system'),
    (2, 'database system'),
    (3, 'database system is a powerful tool'),
    (4, 'system database system');

-- All contain "database system" adjacently; BM25 should rank them
SELECT id, bm25_get_current_score() AS score FROM phrase_rank
WHERE content == 'database system'
ORDER BY score
LIMIT 4;

DROP TABLE phrase_rank;

-- =====================================================
-- Section 8: Mixed with fuzzy search (%=
-- =====================================================
CREATE TABLE phrase_mixed (id int, content text);
CREATE INDEX phrase_mixed_idx ON phrase_mixed USING bm25(content)
    WITH (text_config='simple');

INSERT INTO phrase_mixed VALUES
    (1, 'hello world database'),
    (2, 'hello database world'),
    (3, 'hello world system'),
    (4, 'world hello database');

-- Fuzzy matches all docs containing both words (any order)
SELECT id, bm25_get_current_score() AS score FROM phrase_mixed
WHERE content %= 'hello world'
ORDER BY score
LIMIT 4;

-- Phrase restricts to adjacent only
SELECT id FROM phrase_mixed
WHERE content == 'hello world'
ORDER BY id;

-- == with non-matching phrase should return empty
SELECT id FROM phrase_mixed
WHERE content == 'hello database'
ORDER BY id;

-- Fuzzy still matches docs 1,2,4 (any order)
SELECT id FROM phrase_mixed
WHERE content %= 'hello database'
ORDER BY id;

DROP TABLE phrase_mixed;
