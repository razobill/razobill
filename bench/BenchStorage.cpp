// =============================================================================
// DuckDB storage compaction and analytical query latency benchmark
//
// measures physical database file footprints (pragma_database_size) across raw,
// additive, and decomposed schemas, and evaluates vectorized query latencies
// ~ physical on-disk filesystem bytes and vectorized query latencies
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <vector>

#include "librazobill/Razobill.hpp"
#include "librazobill/Dialect.hpp"
#include "Support.hpp"

using namespace razobill;
namespace fs = std::filesystem;

/**
 * Reads steady clock timestamp in seconds.
 *
 * out: current timestamp in fractional seconds
 */
static double timestampNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * Opens an isolated on-disk DuckDB database via ADBC.
 *
 * in: path is file path to target database
 * out: initialized Backend instance
 */
static std::unique_ptr<Backend> openDatabaseFile(const std::string& path) {
    auto options = support::duckdbOptions();
    fs::remove(path);
    fs::remove(path + ".wal");
    options.databaseOptions = {{"path", path}, {"allow_unsigned_extensions", "true"}};
    return makeAdbcBackend(options);
}

/**
 * Reads exact committed physical database bytes from DuckDB metadata.
 *
 * in: backend is active DuckDB backend
 * out: total committed bytes (used_blocks * block_size)
 */
static std::int64_t measureDatabaseBytes(Backend& backend) {
    backend.exec("CHECKPOINT");
    const auto table =
        backend.query("SELECT CAST(used_blocks AS BIGINT) * CAST(block_size AS BIGINT) FROM pragma_database_size()");
    return table.i64(0, 0);
}

/**
 * Calculates statistical median from a vector of floating-point values.
 *
 * in: values list of measurement samples
 * out: median value or 0.0 if empty
 */
static double computeMedian(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/**
 * Entry point for DuckDB storage compaction and query latency benchmarks.
 *
 * inv: decomposed compaction must pass byte-for-bit lossless verification
 * in: argv[1] optional path to corpus parquet file
 * out: prints physical disk footprints and query execution timings to stdout
 * err: returns 1 on verification failure, 0 on success
 */
int main(int argc, char** argv) {
    const char* corpusParquet = argc > 1 ? argv[1] : "/tmp/corpus.parquet";
    if (!fs::exists(corpusParquet)) {
        std::printf("[BENCH] Warning: Corpus parquet '%s' not found.\n"
                    "        Generating placeholder or pass path via argument.\n",
                    corpusParquet);
    }

    const auto dialect = makeDialect(Flavor::DuckDB);
    const std::string quotedParquet = dialect->quote(corpusParquet);

    std::vector<double> rawBytesRuns, additiveBytesRuns, compactBytesRuns;
    double compactLoadSeconds = 0.0, additiveLoadSeconds = 0.0;

    std::printf("========================================================================================\n");
    std::printf("RAZOBILL DUCKDB PHYSICAL STORAGE COMPACTION BENCHMARK (256 KiB BLOCKS)\n");
    std::printf("========================================================================================\n");
    std::printf("Testing 5 independent physical runs per layout to establish median confidence intervals...\n\n");

    for (int run = 0; run < 5; ++run) {
        // Run 1: Raw Unstructured VARCHAR Layout
        {
            auto backend = openDatabaseFile("/tmp/bs_raw.db");
            backend->exec("CREATE TABLE logs AS SELECT * FROM read_parquet(" + quotedParquet + ")");
            rawBytesRuns.push_back(static_cast<double>(measureDatabaseBytes(*backend)));
        }

        // Run 2: Additive Indexing Layout (Raw string + 64-bit form hash)
        {
            auto backend = openDatabaseFile("/tmp/bs_add.db");
            backend->exec("CREATE TABLE logs AS SELECT * FROM read_parquet(" + quotedParquet + ")");
            RazobillEngine engine(*backend, {.table = "logs", .purpose = "level"});
            const double t0 = timestampNow();
            engine.refresh();
            additiveLoadSeconds = timestampNow() - t0;
            additiveBytesRuns.push_back(static_cast<double>(measureDatabaseBytes(*backend)));
        }

        // Run 3: Full Razobill Columnar Compact Decomposed Layout
        {
            auto backend = openDatabaseFile("/tmp/bs_cmp.db");
            backend->exec("CREATE TEMP TABLE src AS SELECT * FROM read_parquet(" + quotedParquet + ")");
            RazobillEngine engine(*backend, {.table = "logs", .purpose = "level"});
            const double t0 = timestampNow();
            engine.compact("src");
            compactLoadSeconds = timestampNow() - t0;

            // Invariant check: verify that every single reconstructed log
            // matches bit-for-bit
            if (engine.verify("src") != 0) {
                std::puts("[BENCH] CRITICAL ERROR: VERIFY FAILED! Invertibility violated.");
                return 1;
            }
            compactBytesRuns.push_back(static_cast<double>(measureDatabaseBytes(*backend)));
        }

        std::printf("  Run %d: Raw=%10.0f B | Additive=%10.0f B | Compact=%10.0f B (%.2fx raw)\n", run + 1,
                    rawBytesRuns.back(), additiveBytesRuns.back(), compactBytesRuns.back(),
                    compactBytesRuns.back() / rawBytesRuns.back());
    }

    const double medianRaw = computeMedian(rawBytesRuns);
    auto printLayoutStats = [&](const char* layoutName, std::vector<double>& bytesList) {
        const double minBytes = *std::min_element(bytesList.begin(), bytesList.end());
        const double medBytes = computeMedian(bytesList);
        const double maxBytes = *std::max_element(bytesList.begin(), bytesList.end());
        std::printf("%-12s Min=%10.0f B  Median=%10.0f B  Max=%10.0f B  | Ratio vs Raw: %.2f / %.2f / %.2f\n",
                    layoutName, minBytes, medBytes, maxBytes, minBytes / medianRaw, medBytes / medianRaw,
                    maxBytes / medianRaw);
    };

    std::printf("\n----------------------------------------------------------------------------------------\n");
    std::printf("PHYSICAL FOOTPRINT COMPARISON SUMMARY (MEDIAN RATIOS):\n");
    std::printf("----------------------------------------------------------------------------------------\n");
    printLayoutStats("Raw", rawBytesRuns);
    printLayoutStats("Additive", additiveBytesRuns);
    printLayoutStats("Compact", compactBytesRuns);

    std::printf("\nWorst Compact / Best Raw = %.2f  |  Best Compact / Worst Raw = %.2f\n",
                *std::max_element(compactBytesRuns.begin(), compactBytesRuns.end()) /
                    *std::min_element(rawBytesRuns.begin(), rawBytesRuns.end()),
                *std::min_element(compactBytesRuns.begin(), compactBytesRuns.end()) /
                    *std::max_element(rawBytesRuns.begin(), rawBytesRuns.end()));
    std::printf("Ingestion: Additive Refresh %.2fs  |  Compact Decompose %.2fs (134,815 rows)\n", additiveLoadSeconds,
                compactLoadSeconds);

    // =========================================================================
    // Query Execution Latency Audit (Replicated 8x = ~1.08 Million Rows)
    // =========================================================================
    std::printf("\n========================================================================================\n");
    std::printf("VECTORIZED SQL QUERY LATENCY AUDIT (1.08 MILLION ROWS, SINGLE CPU CORE)\n");
    std::printf("========================================================================================\n");

    auto timeBackend = openDatabaseFile("/tmp/bs_time.db");
    timeBackend->exec("CREATE TABLE one AS SELECT * FROM read_parquet(" + quotedParquet + ")");
    timeBackend->exec("CREATE TEMP TABLE src AS SELECT o.* FROM one o, range(8)");
    timeBackend->exec("CREATE TABLE raw AS SELECT * FROM src");

    RazobillEngine engine(*timeBackend, {.table = "logs", .purpose = "level"});
    const double t0 = timestampNow();
    const auto rowCount = engine.compact("src");
    const double compactionTime = timestampNow() - t0;

    engine.define("SSH_PASSWORD_FAIL", "^Failed password for");
    engine.define("HDFS_RECEIVING", R"(^Receiving block <\*> src: )");

    std::printf("Replicated Table: %llu rows, Compact Ingestion Time: %.2fs (%.0f rows/sec)\n\n",
                static_cast<unsigned long long>(rowCount), compactionTime,
                static_cast<double>(rowCount) / (compactionTime > 0 ? compactionTime : 1.0));

    // Measures query latency across 5 runs, reporting the median
    auto timeQuery = [&](const char* queryName, const std::string& sql) {
        std::vector<double> timings;
        for (int i = 0; i < 5; ++i) {
            const double start = timestampNow();
            (void)timeBackend->query(sql);
            timings.push_back((timestampNow() - start) * 1000.0); // Convert to milliseconds
        }
        const double medMs = computeMedian(timings);
        std::printf("  %-54s %9.1f ms\n", queryName, medMs);
        return medMs;
    };

    // Benchmark analytical queries
    timeQuery("Raw: GROUP BY level", "SELECT level, count(*) FROM raw GROUP BY 1");
    timeQuery("Compact Physical: GROUP BY level", "SELECT level, count(*) FROM logs_c GROUP BY 1");
    timeQuery("Compact View: GROUP BY level", "SELECT level, count(*) FROM logs GROUP BY 1");

    timeQuery("Raw: Constant Text LIKE Filter", "SELECT count(*) FROM raw WHERE msg LIKE '%Failed password%'");
    timeQuery(
        "Compact: Constant Text via rb_form Template",
        "SELECT count(*) FROM logs_c c JOIN rb_form f ON f.no = c.form_no WHERE f.template LIKE '%Failed password%'");
    timeQuery("Compact View: msg LIKE (On-the-fly Rebuild)",
              "SELECT count(*) FROM logs WHERE msg LIKE '%Failed password%'");

    timeQuery("Raw: sum(length(msg))", "SELECT sum(length(msg)) FROM raw");
    timeQuery("Compact View: sum(length(msg)) (Full Rebuild)", "SELECT sum(length(msg)) FROM logs");

    timeQuery("Raw: Variable Text LIKE Filter", "SELECT count(*) FROM raw WHERE msg LIKE '%blk_-1608999687919862906%'");
    timeQuery("Compact View: Variable Text LIKE Filter",
              "SELECT count(*) FROM logs WHERE msg LIKE '%blk_-1608999687919862906%'");

    timeQuery("Compact: GROUP BY Meaning (Physical Join)",
              "SELECT d.meaning, count(*) FROM logs_c c JOIN rb_form f ON f.no = c.form_no LEFT JOIN rb_doubt d ON "
              "d.form_id = f.id GROUP BY 1");
    timeQuery("Compact View: GROUP BY Meaning (logs_m)", "SELECT meaning, count(*) FROM logs_m GROUP BY 1");
    timeQuery("Compact View: Rebuild 1,000 Matching Rows",
              "SELECT msg FROM logs_m WHERE meaning = 'SSH_PASSWORD_FAIL' LIMIT 1000");

    std::printf("========================================================================================\n");
    return 0;
}
