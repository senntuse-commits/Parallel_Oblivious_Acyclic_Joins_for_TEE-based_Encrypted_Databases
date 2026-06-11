#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <sgx_urts.h>

#include "Enclave_u.h"
#include "util.h"

using namespace std;

// App.cpp is the untrusted benchmark driver. It owns command-line parsing,
// dataset projection loading, enclave creation, and result/profiling reporting.
namespace
{
// Internal mode ids. The README maps these flags to the report names:
// -JFYan -> JFYan, -ParYan -> ParYan, -ObliYan -> ObliYan.
constexpr int ModeOurs = 0;
constexpr int ModeObliViator = 1;
constexpr int ModeRelaxed = 2;

// Stage indices must stay in sync with Enclave/Enclave.cpp so the app can
// print human-readable names for enclave-side timing arrays.
constexpr int StagePrimitivePhaseBase = 23;
constexpr int PrimitiveKindCount = 4;
constexpr int PrimitivePhaseCount = 9;
constexpr int ProfileStageCount = StagePrimitivePhaseBase + PrimitiveKindCount * PrimitivePhaseCount;
constexpr int StageOursUpFilter = 10;
constexpr int StageOursRootExpand = 11;
constexpr int StageOursTopDown = 12;
constexpr int StageOblivUpFilter = 13;
constexpr int StageOblivDownFilter = 14;
constexpr int StageOblivJoin = 15;
constexpr int StagePrimitiveSort = 16;
constexpr int StagePrimitiveExpand = 17;
constexpr int StagePrimitiveCompact = 18;
constexpr int StagePrimitiveAggtree = 19;
constexpr int StageRelaxedUpFilter = 20;
constexpr int StageRelaxedDownFilter = 21;
constexpr int StageRelaxedJoin = 22;

int globalThreadCount = 16;

void die(const char *msg)
{
    std::fprintf(stderr, "%s\n", msg);
    std::exit(1);
}

const char *modeName(int mode)
{
    switch (mode)
    {
    case ModeOurs:
        return "JFYan";
    case ModeObliViator:
        return "ParYan";
    case ModeRelaxed:
        return "ObliYan";
    default:
        return "unknown";
    }
}

const char *sgxStatusName(sgx_status_t status)
{
    switch (status)
    {
    case SGX_SUCCESS:
        return "SGX_SUCCESS";
    case SGX_ERROR_UNEXPECTED:
        return "SGX_ERROR_UNEXPECTED";
    case SGX_ERROR_INVALID_PARAMETER:
        return "SGX_ERROR_INVALID_PARAMETER";
    case SGX_ERROR_OUT_OF_MEMORY:
        return "SGX_ERROR_OUT_OF_MEMORY";
    case SGX_ERROR_ENCLAVE_LOST:
        return "SGX_ERROR_ENCLAVE_LOST";
    case SGX_ERROR_INVALID_ENCLAVE:
        return "SGX_ERROR_INVALID_ENCLAVE";
    case SGX_ERROR_INVALID_ENCLAVE_ID:
        return "SGX_ERROR_INVALID_ENCLAVE_ID";
    default:
        return "SGX_ERROR_UNKNOWN";
    }
}

const char *primitivePhaseName(int phase)
{
    static const char *names[PrimitivePhaseCount] = {
        "oursUp",
        "oursRootExpand",
        "oursDown",
        "oblivUp",
        "oblivDown",
        "oblivJoin",
        "relaxedUp",
        "relaxedDown",
        "relaxedJoin",
    };
    return (phase >= 0 && phase < PrimitivePhaseCount) ? names[phase] : "unknownPhase";
}

const char *primitiveKindName(int kind)
{
    static const char *names[PrimitiveKindCount] = {
        "sort",
        "expand",
        "compact",
        "aggtree",
    };
    return (kind >= 0 && kind < PrimitiveKindCount) ? names[kind] : "unknownPrimitive";
}

void printUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [-JFYan|-ParYan|-ObliYan|--all] [--bench-only|--profile|--stage-profile] [-t threads] [--thread-sweep list] [-m max_cells] [-tau value]\n"
              << "                  [--sql18 projected_dir|--sql64 projected_dir|--sql72 projected_dir|--sql85 projected_dir|--sql85-chain3 projected_dir|--sql85-returns-star projected_dir|--tpch9 tpch_dir|--tpch-ternary-l3 projected_dir|--tpch-binary-l3 projected_dir|--full-ternary-l3] [--print-limit rows]\n"
              << "Default: sample data, -JFYan -t 16 -m 1000000 --print-limit 20\n"
              << "--bench-only runs the join and returns only rows/cols, avoiding full result copy-out.\n"
              << "--profile also returns detailed enclave-side stage timings for debugging.\n"
              << "--stage-profile returns coarse stage timings without detailed enclave logs.\n"
              << "--thread-sweep reuses one enclave and runs comma-separated thread counts, e.g. 8,16,24,32.\n";
    std::cout << "--do-epsilon and --do-delta set the DO padding privacy parameters when -tau is not provided.\n";
    std::cout << "--materialize-padding makes the join physically output the DO protected row count; --no-materialize-padding runs exact-size join-only profiling.\n";
    std::cout << "--tpch9 enables --materialize-padding by default; SQL18/SQL85 variants keep it off by default.\n";
    std::cout << "--full-ternary-l3 uses --random-rows N --key-range N --seed N|random (defaults: 500, 300, 1).\n";
    std::cout << "--star15 builds a root with 15 leaf children (runTest11 tree); same --random-rows/--key-range/--seed knobs.\n";
    std::cout << "--star4 builds a root with 4 leaf children (5 relations; RRS-feasible, runnable without -tau).\n";
}

// Parse comma-separated thread sweeps such as "8,16,24,32".
std::vector<int> parseThreadList(const std::string &text)
{
    std::vector<int> values;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        int v = std::atoi(item.c_str());
        if (v > 0)
            values.push_back(v);
    }
    return values;
}

// Convert enclave timing slots into stable labels for profile output.
const char *stageName(int mode, int idx)
{
    static std::string dynamicName;
    if (idx >= StagePrimitivePhaseBase && idx < ProfileStageCount)
    {
        int off = idx - StagePrimitivePhaseBase;
        dynamicName = std::string(primitivePhaseName(off / PrimitiveKindCount)) +
                      "." + primitiveKindName(off % PrimitiveKindCount);
        return dynamicName.c_str();
    }

    switch (idx)
    {
    case 0:
        return "restoreTables";
    case 1:
        return "restoreMeta";
    case 2:
        return "setupCopy";
    case 3:
        return mode == ModeOurs ? "bottomUpSemiJoin" :
               mode == ModeObliViator ? "TwoPhaseFilter" : "RelaxedJoin";
    case 4:
        return mode == ModeOurs ? "topDown" :
               mode == ModeObliViator ? "JoinByObliViator" : "unused";
    case 5:
        return "summarize";
    case 6:
        return "insideTotal";
    case 7:
        return "doExactRows";
    case 8:
        return "doSensitivity";
    case 9:
        return "doProtectedRows";
    case StageOursUpFilter:
        return "oursUpFilter";
    case StageOursRootExpand:
        return "oursRootExpand";
    case StageOursTopDown:
        return "oursTopDown";
    case StageOblivUpFilter:
        return "oblivUpFilter";
    case StageOblivDownFilter:
        return "oblivDownFilter";
    case StageOblivJoin:
        return "oblivJoin";
    case StagePrimitiveSort:
        return "primitiveSort";
    case StagePrimitiveExpand:
        return "primitiveExpand";
    case StagePrimitiveCompact:
        return "primitiveCompact";
    case StagePrimitiveAggtree:
        return "primitiveAggtree";
    case StageRelaxedUpFilter:
        return "relaxedUpFilter";
    case StageRelaxedDownFilter:
        return "relaxedDownFilter";
    case StageRelaxedJoin:
        return "relaxedJoin";
    default:
        return "unknown";
    }
}

bool stageIsMetric(int idx)
{
    return idx >= 7 && idx <= 9;
}

std::vector<Table> sampleTables()
{
    Table r1 = {{15, 10, 3}, {8, 7, 5}, {9, 10, 1}, {8, 9, 3}};
    Table r2 = {{10, 8}, {11, 9}, {12, 8}, {13, 15}};
    Table r3 = {{7, 15}, {7, 16}, {10, 17}, {9, 18}};
    Table r4 = {{1, 1}, {3, 4}, {3, 2}, {5, 1}};
    Table r5 = {{1, 1}, {3, 2}, {3, 3}, {1, 4}};
    Table r6 = {{1, 5}, {2, 6}, {4, 7}, {4, 8}};
    return {r1, r2, r3, r4, r5, r6};
}

void makeFullTernaryL3(std::vector<Table> &tables,
                       std::vector<int> &parent,
                       std::vector<int> &tableKeys,
                       std::vector<int> &joinColInParent,
                       std::vector<int> &joinColInChild,
                       int rows,
                       int keyRange,
                       int seed)
{
    constexpr int arity = 3;
    constexpr int levels = 3;
    int n = 0;
    int levelSize = 1;
    for (int level = 0; level < levels; ++level)
    {
        n += levelSize;
        levelSize *= arity;
    }

    parent.assign(n, -1);
    tableKeys.assign(n, 2);
    joinColInParent.assign(n, -1);
    joinColInChild.assign(n, -1);

    for (int p = 0; p < n; ++p)
    {
        int firstChild = arity * p + 1;
        bool isInternal = firstChild < n;
        tableKeys[p] = isInternal ? arity : 2;
        for (int j = 0; j < arity; ++j)
        {
            int c = firstChild + j;
            if (c >= n)
                break;
            parent[c] = p;
            joinColInParent[c] = j;
            joinColInChild[c] = 0;
        }
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(1, keyRange);
    tables.assign(n, Table());
    for (int t = 0; t < n; ++t)
        tables[t].reserve(rows);

    for (int r = 0; r < rows; ++r)
    {
        for (int t = 0; t < n; ++t)
        {
            std::vector<int> tuple(tableKeys[t]);
            for (int k = 0; k < tableKeys[t]; ++k)
                tuple[k] = dist(rng);
            tables[t].push_back(std::move(tuple));
        }
    }
}

// Star tree: a single root with `childCount` leaf children. The root has
// `childCount` columns (one join key per child); every child is a 2-column leaf
// joining root.col(c-1) to child.col0. Same random fill as makeFullTernaryL3
// (rows per table, values in [1, keyRange]). childCount=15 mirrors runTest11.
void makeStarTree(int childCount,
                  std::vector<Table> &tables,
                  std::vector<int> &parent,
                  std::vector<int> &tableKeys,
                  std::vector<int> &joinColInParent,
                  std::vector<int> &joinColInChild,
                  int rows,
                  int keyRange,
                  int seed)
{
    const int n = 1 + childCount; // root + leaves

    parent.assign(n, -1);
    tableKeys.assign(n, 2);
    joinColInParent.assign(n, -1);
    joinColInChild.assign(n, -1);

    tableKeys[0] = childCount; // root carries one column per child
    for (int c = 1; c < n; ++c)
    {
        parent[c] = 0;
        joinColInParent[c] = c - 1; // root column c-1 joins child c
        joinColInChild[c] = 0;      // child joins on its first column
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(1, keyRange);
    tables.assign(n, Table());
    for (int t = 0; t < n; ++t)
        tables[t].reserve(rows);

    for (int r = 0; r < rows; ++r)
    {
        for (int t = 0; t < n; ++t)
        {
            std::vector<int> tuple(tableKeys[t]);
            for (int k = 0; k < tableKeys[t]; ++k)
                tuple[k] = dist(rng);
            tables[t].push_back(std::move(tuple));
        }
    }
}

std::string pathJoin(const std::string &dir, const char *file)
{
    if (dir.empty())
        return file;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\')
        return dir + file;
    return dir + "/" + file;
}

Table loadRequiredTable(const std::string &path, int expectedCols)
{
    Table table = loadData(path);
    if (table.empty())
    {
        std::string msg = "Cannot load table or table is empty: " + path;
        die(msg.c_str());
    }
    for (int i = 0; i < (int)table.size(); ++i)
    {
        if ((int)table[i].size() != expectedCols)
        {
            std::fprintf(stderr, "Bad column count in %s at row %d: got %d, expected %d\n",
                         path.c_str(), i, (int)table[i].size(), expectedCols);
            std::exit(1);
        }
    }
    return table;
}

std::vector<Table> loadSQL18Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_catalog_sales.tbl"), 4),
        loadRequiredTable(pathJoin(dir, "R2_date_dim.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_item.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_cd1.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R5_customer.tbl"), 3),
        loadRequiredTable(pathJoin(dir, "R6_cd2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R7_customer_address.tbl"), 2),
    };
}

std::vector<Table> loadSQL64Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_customer.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R2_customer_demographics.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_store_returns.tbl"), 2),
    };
}

std::vector<Table> loadSQL72Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_catalog_sales.tbl"), 7),
        loadRequiredTable(pathJoin(dir, "R2_item.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_customer_demographics.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_household_demographics.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R5_date_dim_d1.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R6_date_dim_d3.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R7_promotion.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R8_catalog_returns.tbl"), 2),
    };
}

std::vector<Table> loadSQL85Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_web_sales.tbl"), 3),
        loadRequiredTable(pathJoin(dir, "R2_web_page.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_date_dim.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_web_returns.tbl"), 5),
        loadRequiredTable(pathJoin(dir, "R5_cd1.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R6_cd2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R7_customer_address.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R8_reason.tbl"), 2),
    };
}

std::vector<Table> loadSQL85Chain3Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_web_sales.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R2_web_returns.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_cd1.tbl"), 2),
    };
}

std::vector<Table> loadSQL85ReturnsStarTables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_web_returns.tbl"), 4),
        loadRequiredTable(pathJoin(dir, "R2_cd1.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_cd2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_customer_address.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R5_reason.tbl"), 2),
    };
}

static long long compositePairKey(int a, int b)
{
    return (static_cast<long long>(a) << 32) ^ static_cast<unsigned int>(b);
}

static int compositePairId(std::unordered_map<long long, int> &ids, int a, int b)
{
    long long key = compositePairKey(a, b);
    auto it = ids.find(key);
    if (it != ids.end())
        return it->second;
    int id = (int)ids.size() + 1;
    ids.emplace(key, id);
    return id;
}

std::vector<Table> loadTPCH9Tables(const std::string &dir)
{
    Table orders = loadRequiredTable(pathJoin(dir, "orders_ok.txt"), 1);
    Table lineitemRaw = loadRequiredTable(pathJoin(dir, "lineitem_sk_pk_ok.txt"), 3);
    Table partsuppRaw = loadRequiredTable(pathJoin(dir, "partsupp.txt"), 2);
    Table part = loadRequiredTable(pathJoin(dir, "part.txt"), 1);
    Table supplier = loadRequiredTable(pathJoin(dir, "supplier_sk_nk.txt"), 2);
    Table nation = loadRequiredTable(pathJoin(dir, "nation.txt"), 1);

    std::unordered_map<long long, int> pairIds;
    pairIds.reserve((lineitemRaw.size() + partsuppRaw.size()) * 2 + 1);

    Table lineitem;
    lineitem.reserve(lineitemRaw.size());
    for (const auto &row : lineitemRaw)
    {
        int suppKey = row[0];
        int partKey = row[1];
        int orderKey = row[2];
        int pairKey = compositePairId(pairIds, suppKey, partKey);
        lineitem.push_back({orderKey, pairKey});
    }

    Table partsupp;
    partsupp.reserve(partsuppRaw.size());
    for (const auto &row : partsuppRaw)
    {
        int suppKey = row[0];
        int partKey = row[1];
        int pairKey = compositePairId(pairIds, suppKey, partKey);
        partsupp.push_back({pairKey, partKey, suppKey});
    }

    return {lineitem, orders, partsupp, part, supplier, nation};
}

std::vector<Table> loadTPCHTernaryL3Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_lineitem.tbl"), 3),
        loadRequiredTable(pathJoin(dir, "R2_orders.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_part.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_supplier.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R5_customer.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R6_orders2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R7_customer2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R8_part2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R9_partsupp.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R10_part3.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R11_nation.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R12_supplier2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R13_nation2.tbl"), 2),
    };
}

std::vector<Table> loadTPCHBinaryL3Tables(const std::string &dir)
{
    return {
        loadRequiredTable(pathJoin(dir, "R1_lineitem.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R2_orders.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R3_partsupp.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R4_customer.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R5_orders2.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R6_part.tbl"), 2),
        loadRequiredTable(pathJoin(dir, "R7_supplier.tbl"), 2),
    };
}

long long loadExpectedRows(const std::string &dir)
{
    std::ifstream in(pathJoin(dir, "expected.txt"));
    long long expected = -1;
    if (in)
        in >> expected;
    return expected;
}

struct DatasetConfig
{
    std::vector<Table> tables;
    std::vector<int> parent;
    std::vector<int> tableKeys;
    std::vector<int> joinColInParent;
    std::vector<int> joinColInChild;
    int root = 0;
    long long expectedRows = -1;
};

DatasetConfig loadSQL18Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL18Tables(dir);
    cfg.parent = {-1, 0, 0, 0, 0, 4, 4};
    cfg.tableKeys = {4, 2, 2, 2, 3, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 2, 3, 1, 2};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadSQL64Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL64Tables(dir);
    cfg.parent = {-1, 0, 1};
    cfg.tableKeys = {2, 2, 2};
    cfg.joinColInParent = {-1, 0, 0};
    cfg.joinColInChild = {-1, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadSQL72Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL72Tables(dir);
    cfg.parent = {-1, 0, 0, 0, 0, 0, 0, 0};
    cfg.tableKeys = {7, 2, 2, 2, 2, 2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 2, 3, 4, 5, 6};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadSQL85Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL85Tables(dir);
    cfg.parent = {-1, 0, 0, 0, 3, 3, 3, 3};
    cfg.tableKeys = {3, 2, 2, 5, 2, 2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 2, 1, 2, 3, 4};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadSQL85Chain3Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL85Chain3Tables(dir);
    cfg.parent = {-1, 0, 1};
    cfg.tableKeys = {2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1};
    cfg.joinColInChild = {-1, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadSQL85ReturnsStarDataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadSQL85ReturnsStarTables(dir);
    cfg.parent = {-1, 0, 0, 0, 0};
    cfg.tableKeys = {4, 2, 2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 2, 3};
    cfg.joinColInChild = {-1, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadTPCH9Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadTPCH9Tables(dir);
    cfg.parent = {-1, 0, 0, 2, 2, 4};
    cfg.tableKeys = {2, 1, 3, 1, 2, 1};
    cfg.joinColInParent = {-1, 0, 1, 1, 2, 1};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadTPCHTernaryL3Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadTPCHTernaryL3Tables(dir);
    cfg.parent = {-1, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3};
    cfg.tableKeys = {3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 2, 1, 1, 1, 0, 0, 0, 1, 1, 1};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

DatasetConfig loadTPCHBinaryL3Dataset(const std::string &dir)
{
    DatasetConfig cfg;
    cfg.tables = loadTPCHBinaryL3Tables(dir);
    cfg.parent = {-1, 0, 0, 1, 1, 2, 2};
    cfg.tableKeys = {2, 2, 2, 2, 2, 2, 2};
    cfg.joinColInParent = {-1, 0, 1, 1, 1, 0, 1};
    cfg.joinColInChild = {-1, 0, 0, 0, 0, 0, 0};
    cfg.expectedRows = loadExpectedRows(dir);
    return cfg;
}

struct RunSummary
{
    int mode = ModeOurs;
    int rows = 0;
    int cols = 0;
    double ms = 0.0;
    sgx_status_t callStatus = SGX_SUCCESS;
    sgx_status_t ecallStatus = SGX_SUCCESS;
    std::vector<double> stageMs;
    bool paddingStatsOnly = false;
    bool skipped = false;
    bool hasResultSignature = false;
    std::uint64_t resultSignature = 0;
    long long resultCellCount = 0;
};

long long stageMetric(const RunSummary &s, int idx)
{
    if (idx < 0 || idx >= (int)s.stageMs.size())
        return -1;
    return (long long)s.stageMs[idx];
}

void printDOPaddingSummary(const RunSummary &s, const char *prefix)
{
    long long exactRows = stageMetric(s, 7);
    long long protectedRows = stageMetric(s, 9);
    if (exactRows < 0 || protectedRows < 0)
        return;
    long long paddingRows = protectedRows > exactRows ? protectedRows - exactRows : 0;
    std::cout << prefix << "DO output rows: real=" << exactRows
              << " protected=" << protectedRows
              << " padding=" << paddingRows << "\n";
}

bool isDOPaddingStatsOnly(const RunSummary &s)
{
    constexpr long long MaxMaterializedProtectedRows = 50000000;
    long long exactRows = stageMetric(s, 7);
    long long protectedRows = stageMetric(s, 9);
    return s.rows == 0 && s.cols == 0 &&
           exactRows > 0 && protectedRows > MaxMaterializedProtectedRows;
}

bool usedExactRowsInsteadOfProtectedRows(const RunSummary &s)
{
    long long exactRows = stageMetric(s, 7);
    long long protectedRows = stageMetric(s, 9);
    return exactRows > 0 && protectedRows > exactRows &&
           s.rows == exactRows && s.rows > 0;
}

void printModeBanner(const char *label, int threads)
{
    std::cout << "\n==============================\n"
              << "Mode: " << label << "  threads=" << threads << "\n"
              << "==============================\n";
}

double joinOnlyMs(const RunSummary &s)
{
    if ((int)s.stageMs.size() > 4)
        return s.stageMs[3] + s.stageMs[4];
    return s.ms;
}

double stageTime(const RunSummary &s, int idx)
{
    if (idx < 0 || idx >= (int)s.stageMs.size())
        return 0.0;
    return s.stageMs[idx];
}

double oursFullCompareMs(const RunSummary &s)
{
    return stageTime(s, StageOursUpFilter) +
           stageTime(s, StageOursRootExpand) +
           stageTime(s, StageOursTopDown);
}

double oursWithoutFirstUpMs(const RunSummary &s)
{
    return stageTime(s, StageOursRootExpand) + stageTime(s, StageOursTopDown);
}

double oblivWithoutFirstUpMs(const RunSummary &s)
{
    return stageTime(s, StageOblivDownFilter) + stageTime(s, StageOblivJoin);
}

int primitivePhaseStageIndex(int phase, int kind)
{
    return StagePrimitivePhaseBase + phase * PrimitiveKindCount + kind;
}

void printPrimitivePhaseRow(const RunSummary &s, const char *prefix, const char *label, int phase)
{
    std::cout << prefix << label
              << ": sort=" << stageTime(s, primitivePhaseStageIndex(phase, 0))
              << " ms  expand=" << stageTime(s, primitivePhaseStageIndex(phase, 1))
              << " ms  compact=" << stageTime(s, primitivePhaseStageIndex(phase, 2))
              << " ms  aggtree=" << stageTime(s, primitivePhaseStageIndex(phase, 3))
              << " ms\n";
}

void printPrimitiveCombinedPhaseRow(const RunSummary &s, const char *prefix, const char *label, int phaseA, int phaseB)
{
    std::cout << prefix << label
              << ": sort=" << stageTime(s, primitivePhaseStageIndex(phaseA, 0)) + stageTime(s, primitivePhaseStageIndex(phaseB, 0))
              << " ms  expand=" << stageTime(s, primitivePhaseStageIndex(phaseA, 1)) + stageTime(s, primitivePhaseStageIndex(phaseB, 1))
              << " ms  compact=" << stageTime(s, primitivePhaseStageIndex(phaseA, 2)) + stageTime(s, primitivePhaseStageIndex(phaseB, 2))
              << " ms  aggtree=" << stageTime(s, primitivePhaseStageIndex(phaseA, 3)) + stageTime(s, primitivePhaseStageIndex(phaseB, 3))
              << " ms\n";
}

void printPrimitiveSummaryForMode(const RunSummary &s, const char *prefix)
{
    std::cout << prefix << modeName(s.mode) << " primitives:"
              << " sort=" << stageTime(s, StagePrimitiveSort)
              << " ms  expand=" << stageTime(s, StagePrimitiveExpand)
              << " ms  compact=" << stageTime(s, StagePrimitiveCompact)
              << " ms  aggtree=" << stageTime(s, StagePrimitiveAggtree)
              << " ms\n";
}

void printPrimitivePhaseSummaryForMode(const RunSummary &s, const char *prefix)
{
    if (s.mode == ModeOurs)
    {
        printPrimitivePhaseRow(s, prefix, "ours.upFilter", 0);
        printPrimitiveCombinedPhaseRow(s, prefix, "ours.down", 1, 2);
    }
    else if (s.mode == ModeObliViator)
    {
        printPrimitivePhaseRow(s, prefix, "obliv.upFilter", 3);
        printPrimitivePhaseRow(s, prefix, "obliv.downFilter", 4);
        printPrimitivePhaseRow(s, prefix, "obliv.join", 5);
    }
    else if (s.mode == ModeRelaxed)
    {
        printPrimitivePhaseRow(s, prefix, "relaxed.upFilter", 6);
        printPrimitivePhaseRow(s, prefix, "relaxed.downFilter", 7);
        printPrimitivePhaseRow(s, prefix, "relaxed.join", 8);
    }
}

void printComparisonSummary(const std::vector<RunSummary> &summaries, const char *prefix)
{
    const RunSummary *ours = nullptr;
    const RunSummary *obliv = nullptr;
    const RunSummary *relaxed = nullptr;
    for (const RunSummary &s : summaries)
    {
        if (s.mode == ModeOurs)
            ours = &s;
        else if (s.mode == ModeObliViator)
            obliv = &s;
        else if (s.mode == ModeRelaxed)
            relaxed = &s;
    }
    if (!ours)
        return;

    double oursFull = oursFullCompareMs(*ours);
    double oursNoFirstUp = oursWithoutFirstUpMs(*ours);
    if (oursFull <= 0.0)
        oursFull = joinOnlyMs(*ours);

    std::cout << prefix << "JFYan: upFilter=" << stageTime(*ours, StageOursUpFilter)
              << " ms  down=" << oursNoFirstUp
              << " ms  full=" << oursFull << " ms\n";

    if (obliv)
    {
        double oblivNoFirstUp = oblivWithoutFirstUpMs(*obliv);
        std::cout << prefix << "ParYan: upFilter=" << stageTime(*obliv, StageOblivUpFilter)
                  << " ms  downFilter=" << stageTime(*obliv, StageOblivDownFilter)
                  << " ms  upJoin=" << stageTime(*obliv, StageOblivJoin)
                  << " ms  compare=" << oblivNoFirstUp << " ms";
        if (oursNoFirstUp > 0.0 && oblivNoFirstUp > 0.0)
            std::cout << "  ratio=" << (oblivNoFirstUp / oursNoFirstUp) << "x";
        std::cout << "\n";
    }

    if (relaxed)
    {
        double relaxedMs = joinOnlyMs(*relaxed);
        std::cout << prefix << "ObliYan: upFilter=" << stageTime(*relaxed, StageRelaxedUpFilter)
                  << " ms  downFilter=" << stageTime(*relaxed, StageRelaxedDownFilter)
                  << " ms  join=" << stageTime(*relaxed, StageRelaxedJoin)
                  << " ms  full=" << relaxedMs << " ms";
        if (oursFull > 0.0 && relaxedMs > 0.0)
            std::cout << "  ratio=" << (relaxedMs / oursFull) << "x";
        std::cout << "\n";
    }

    if (relaxed || obliv)
    {
        std::cout << prefix << "Ratios:";
        if (relaxed && oursFull > 0.0)
        {
            double relaxedMs = joinOnlyMs(*relaxed);
            if (relaxedMs > 0.0)
                std::cout << " ObliYan/JFYan=" << (relaxedMs / oursFull) << "x";
        }
        if (relaxed && obliv)
        {
            double relaxedMs = joinOnlyMs(*relaxed);
            double oblivTotal = joinOnlyMs(*obliv);
            if (relaxedMs > 0.0 && oblivTotal > 0.0)
                std::cout << " ObliYan/ParYan(total)=" << (relaxedMs / oblivTotal) << "x";
        }
        if (obliv)
        {
            double oblivDownJoin = oblivWithoutFirstUpMs(*obliv);
            if (oblivDownJoin > 0.0 && oursNoFirstUp > 0.0)
                std::cout << " ParYan(down+join)/JFYan(down)="
                          << (oblivDownJoin / oursNoFirstUp) << "x";
        }
        std::cout << "\n";
    }

    if (ours || obliv || relaxed)
    {
        std::cout << prefix << "Primitive totals:\n";
        if (ours)
            printPrimitiveSummaryForMode(*ours, (std::string(prefix) + "  ").c_str());
        if (obliv)
            printPrimitiveSummaryForMode(*obliv, (std::string(prefix) + "  ").c_str());
        if (relaxed)
            printPrimitiveSummaryForMode(*relaxed, (std::string(prefix) + "  ").c_str());
        std::cout << prefix << "Primitive by stage:\n";
        if (ours)
            printPrimitivePhaseSummaryForMode(*ours, (std::string(prefix) + "  ").c_str());
        if (obliv)
            printPrimitivePhaseSummaryForMode(*obliv, (std::string(prefix) + "  ").c_str());
        if (relaxed)
            printPrimitivePhaseSummaryForMode(*relaxed, (std::string(prefix) + "  ").c_str());
    }
}

RunSummary runMode(sgx_enclave_id_t eid,
                   int threads,
                   int mode,
                   int tauOrOutSize,
                   const FlatTables &flat,
                   int tableCount,
                   const std::vector<int> &parent,
                   int root,
                   const std::vector<int> &joinColInParent,
                   const std::vector<int> &joinColInChild,
                   const std::vector<int> &tableKeys,
                   std::vector<int> &result,
                   double doEpsilon,
                   double doDelta,
                   bool materializePadding,
                   bool benchmarkOnly,
                   bool profile,
                   bool stageProfile)
{
    RunSummary summary;
    summary.mode = mode;

    if (!benchmarkOnly && !profile)
        std::fill(result.begin(), result.end(), 0);
    auto t0 = std::chrono::high_resolution_clock::now();
    if (profile || stageProfile)
    {
        summary.stageMs.assign(ProfileStageCount, 0.0);
        if (profile)
        {
            summary.callStatus = AcyclicJoinRunProfile(eid,
                                                       &summary.ecallStatus,
                                                       threads,
                                                       mode,
                                                       const_cast<int *>(flat.data.data()),
                                                       (int)flat.data.size(),
                                                       const_cast<int *>(flat.offsets.data()),
                                                       const_cast<int *>(flat.rows.data()),
                                                       const_cast<int *>(flat.cols.data()),
                                                       tableCount,
                                                       const_cast<int *>(parent.data()),
                                                       root,
                                                       const_cast<int *>(joinColInParent.data()),
                                                       const_cast<int *>(joinColInChild.data()),
                                                       const_cast<int *>(tableKeys.data()),
                                                       tauOrOutSize,
                                                       materializePadding ? 1 : 0,
                                                       doEpsilon,
                                                       doDelta,
                                                       &summary.rows,
                                                       &summary.cols,
                                                       summary.stageMs.data(),
                                                       (int)summary.stageMs.size());
        }
        else
        {
            summary.callStatus = AcyclicJoinRunStageSummary(eid,
                                                            &summary.ecallStatus,
                                                            threads,
                                                            mode,
                                                            const_cast<int *>(flat.data.data()),
                                                            (int)flat.data.size(),
                                                            const_cast<int *>(flat.offsets.data()),
                                                            const_cast<int *>(flat.rows.data()),
                                                            const_cast<int *>(flat.cols.data()),
                                                            tableCount,
                                                            const_cast<int *>(parent.data()),
                                                            root,
                                                            const_cast<int *>(joinColInParent.data()),
                                                            const_cast<int *>(joinColInChild.data()),
                                                            const_cast<int *>(tableKeys.data()),
                                                            tauOrOutSize,
                                                            materializePadding ? 1 : 0,
                                                            doEpsilon,
                                                            doDelta,
                                                            &summary.rows,
                                                            &summary.cols,
                                                            summary.stageMs.data(),
                                                            (int)summary.stageMs.size());
        }
    }
    else if (benchmarkOnly)
    {
        summary.callStatus = AcyclicJoinRunSummary(eid,
                                                   &summary.ecallStatus,
                                                   threads,
                                                   mode,
                                                   const_cast<int *>(flat.data.data()),
                                                   (int)flat.data.size(),
                                                   const_cast<int *>(flat.offsets.data()),
                                                   const_cast<int *>(flat.rows.data()),
                                                   const_cast<int *>(flat.cols.data()),
                                                   tableCount,
                                                   const_cast<int *>(parent.data()),
                                                   root,
                                                   const_cast<int *>(joinColInParent.data()),
                                                   const_cast<int *>(joinColInChild.data()),
                                                   const_cast<int *>(tableKeys.data()),
                                                   tauOrOutSize,
                                                   materializePadding ? 1 : 0,
                                                   doEpsilon,
                                                   doDelta,
                                                   &summary.rows,
                                                   &summary.cols);
    }
    else
    {
        summary.callStatus = AcyclicJoinRun(eid,
                                            &summary.ecallStatus,
                                            threads,
                                            mode,
                                            const_cast<int *>(flat.data.data()),
                                            (int)flat.data.size(),
                                            const_cast<int *>(flat.offsets.data()),
                                            const_cast<int *>(flat.rows.data()),
                                            const_cast<int *>(flat.cols.data()),
                                            tableCount,
                                            const_cast<int *>(parent.data()),
                                            root,
                                            const_cast<int *>(joinColInParent.data()),
                                            const_cast<int *>(joinColInChild.data()),
                                            const_cast<int *>(tableKeys.data()),
                                            tauOrOutSize,
                                            materializePadding ? 1 : 0,
                                            doEpsilon,
                                            doDelta,
                                            result.data(),
                                            (int)result.size(),
                                            &summary.rows,
                                            &summary.cols);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    summary.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return summary;
}

sgx_enclave_id_t createEnclave()
{
    sgx_enclave_id_t eid = 0;
    const char *signedSo = "build/enclave.signed.so";
    sgx_status_t ret = sgx_create_enclave(signedSo, SGX_DEBUG_FLAG, nullptr, nullptr, &eid, nullptr);
    if (ret != SGX_SUCCESS)
    {
        signedSo = "enclave.signed.so";
        ret = sgx_create_enclave(signedSo, SGX_DEBUG_FLAG, nullptr, nullptr, &eid, nullptr);
    }
    if (ret != SGX_SUCCESS)
    {
        std::fprintf(stderr, "sgx_create_enclave failed: %#x\n", ret);
        die("Make sure enclave.signed.so is in the project root or build/enclave.signed.so exists.");
    }
    return eid;
}
}

void ocall_print(const char *s)
{
    std::printf("%s\n", s);
}

void ocall_now_ms(double *out_ms)
{
    if (!out_ms)
        return;
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    *out_ms = std::chrono::duration<double, std::milli>(now).count();
}

int main(int argc, char **argv)
{
    int mode = ModeOurs;
    bool runAll = false;
    bool benchmarkOnly = false;
    bool profile = false;
    bool stageProfile = false;
    int maxCells = 1000000;
    int tauOrOutSize = 0;
    int printLimit = 20;
    std::string sql18Dir;
    std::string sql64Dir;
    std::string sql72Dir;
    std::string sql85Dir;
    std::string sql85Chain3Dir;
    std::string sql85ReturnsStarDir;
    std::string tpch9Dir;
    std::string tpchTernaryL3Dir;
    std::string tpchBinaryL3Dir;
    bool useFullTernaryL3 = false;
    bool useStar15 = false;
    bool useStar4 = false;
    int randomRows = 500;
    int randomKeyRange = 300;
    int randomSeed = 1;
    bool randomizeSeed = false;
    double doEpsilon = 1.0;
    double doDelta = 1e-9;
    bool materializePadding = false;
    bool materializePaddingExplicit = false;
    std::vector<int> threadSweep;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-JFYan") == 0)
            mode = ModeOurs;
        else if (std::strcmp(argv[i], "-ParYan") == 0)
            mode = ModeObliViator;
        else if (std::strcmp(argv[i], "-ObliYan") == 0)
            mode = ModeRelaxed;
        else if (std::strcmp(argv[i], "--all") == 0)
            runAll = true;
        else if (std::strcmp(argv[i], "--bench-only") == 0 || std::strcmp(argv[i], "--no-result") == 0)
            benchmarkOnly = true;
        else if (std::strcmp(argv[i], "--profile") == 0)
        {
            profile = true;
            benchmarkOnly = true;
        }
        else if (std::strcmp(argv[i], "--stage-profile") == 0)
        {
            stageProfile = true;
            benchmarkOnly = true;
        }
        else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            globalThreadCount = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--thread-sweep") == 0 && i + 1 < argc)
            threadSweep = parseThreadList(argv[++i]);
        else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            maxCells = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-tau") == 0 && i + 1 < argc)
            tauOrOutSize = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--do-epsilon") == 0 && i + 1 < argc)
            doEpsilon = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--do-delta") == 0 && i + 1 < argc)
            doDelta = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--materialize-padding") == 0)
        {
            materializePadding = true;
            materializePaddingExplicit = true;
        }
        else if (std::strcmp(argv[i], "--no-materialize-padding") == 0)
        {
            materializePadding = false;
            materializePaddingExplicit = true;
        }
        else if (std::strcmp(argv[i], "--sql18") == 0 && i + 1 < argc)
            sql18Dir = argv[++i];
        else if (std::strcmp(argv[i], "--sql64") == 0 && i + 1 < argc)
            sql64Dir = argv[++i];
        else if (std::strcmp(argv[i], "--sql72") == 0 && i + 1 < argc)
            sql72Dir = argv[++i];
        else if (std::strcmp(argv[i], "--sql85") == 0 && i + 1 < argc)
            sql85Dir = argv[++i];
        else if (std::strcmp(argv[i], "--sql85-chain3") == 0 && i + 1 < argc)
            sql85Chain3Dir = argv[++i];
        else if (std::strcmp(argv[i], "--sql85-returns-star") == 0 && i + 1 < argc)
            sql85ReturnsStarDir = argv[++i];
        else if (std::strcmp(argv[i], "--tpch9") == 0 && i + 1 < argc)
            tpch9Dir = argv[++i];
        else if (std::strcmp(argv[i], "--tpch-ternary-l3") == 0 && i + 1 < argc)
            tpchTernaryL3Dir = argv[++i];
        else if (std::strcmp(argv[i], "--tpch-binary-l3") == 0 && i + 1 < argc)
            tpchBinaryL3Dir = argv[++i];
        else if (std::strcmp(argv[i], "--full-ternary-l3") == 0)
            useFullTernaryL3 = true;
        else if (std::strcmp(argv[i], "--star15") == 0)
            useStar15 = true;
        else if (std::strcmp(argv[i], "--star4") == 0)
            useStar4 = true;
        else if (std::strcmp(argv[i], "--random-rows") == 0 && i + 1 < argc)
            randomRows = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--key-range") == 0 && i + 1 < argc)
            randomKeyRange = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
        {
            const char *seedArg = argv[++i];
            if (std::strcmp(seedArg, "random") == 0)
                randomizeSeed = true;
            else
            {
                randomSeed = std::atoi(seedArg);
                randomizeSeed = false;
            }
        }
        else if (std::strcmp(argv[i], "--print-limit") == 0 && i + 1 < argc)
            printLimit = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (globalThreadCount <= 0 || maxCells <= 0 || randomRows <= 0 || randomKeyRange <= 0)
    {
        std::cerr << "threads, max_cells, random_rows, and key_range must be positive.\n";
        return 1;
    }
    if (randomizeSeed)
    {
        std::random_device rd;
        randomSeed = (int)(rd() & 0x7fffffff);
        if (randomSeed == 0)
            randomSeed = 1;
    }
    for (int t : threadSweep)
    {
        if (t <= 0)
        {
            std::cerr << "thread sweep values must be positive.\n";
            return 1;
        }
    }
    if (doEpsilon <= 0.0 || doDelta <= 0.0 || doDelta >= 1.0)
    {
        std::cerr << "--do-epsilon must be > 0 and --do-delta must be in (0, 1).\n";
        return 1;
    }

    int datasetCount = (!sql18Dir.empty() ? 1 : 0) +
                       (!sql64Dir.empty() ? 1 : 0) +
                       (!sql72Dir.empty() ? 1 : 0) +
                       (!sql85Dir.empty() ? 1 : 0) +
                       (!sql85Chain3Dir.empty() ? 1 : 0) +
                       (!sql85ReturnsStarDir.empty() ? 1 : 0) +
                       (!tpch9Dir.empty() ? 1 : 0) +
                       (!tpchTernaryL3Dir.empty() ? 1 : 0) +
                       (!tpchBinaryL3Dir.empty() ? 1 : 0) +
                       (useFullTernaryL3 ? 1 : 0) + (useStar15 ? 1 : 0) + (useStar4 ? 1 : 0);
    if (datasetCount > 1)
    {
        std::cerr << "Use only one dataset option: --sql18, --sql64, --sql72, --sql85, --sql85-chain3, --sql85-returns-star, --tpch9, --tpch-ternary-l3, --tpch-binary-l3, --full-ternary-l3, --star15, or --star4.\n";
        return 1;
    }
    if (!tpch9Dir.empty() && !materializePaddingExplicit)
        materializePadding = true;

    std::vector<int> modes;
    if (runAll)
        modes = {ModeOurs, ModeObliViator, ModeRelaxed};
    else
        modes = {mode};

    std::vector<Table> tables;
    std::vector<int> parent;
    std::vector<int> tableKeys;
    std::vector<int> joinColInParent;
    std::vector<int> joinColInChild;
    int root = 0;
    long long expectedRows = -1;

    if (!sql18Dir.empty())
    {
        DatasetConfig cfg = loadSQL18Dataset(sql18Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!sql64Dir.empty())
    {
        DatasetConfig cfg = loadSQL64Dataset(sql64Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!sql72Dir.empty())
    {
        DatasetConfig cfg = loadSQL72Dataset(sql72Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!sql85Dir.empty())
    {
        DatasetConfig cfg = loadSQL85Dataset(sql85Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!sql85Chain3Dir.empty())
    {
        DatasetConfig cfg = loadSQL85Chain3Dataset(sql85Chain3Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!sql85ReturnsStarDir.empty())
    {
        DatasetConfig cfg = loadSQL85ReturnsStarDataset(sql85ReturnsStarDir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!tpch9Dir.empty())
    {
        DatasetConfig cfg = loadTPCH9Dataset(tpch9Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!tpchTernaryL3Dir.empty())
    {
        DatasetConfig cfg = loadTPCHTernaryL3Dataset(tpchTernaryL3Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (!tpchBinaryL3Dir.empty())
    {
        DatasetConfig cfg = loadTPCHBinaryL3Dataset(tpchBinaryL3Dir);
        tables = std::move(cfg.tables);
        parent = std::move(cfg.parent);
        tableKeys = std::move(cfg.tableKeys);
        joinColInParent = std::move(cfg.joinColInParent);
        joinColInChild = std::move(cfg.joinColInChild);
        root = cfg.root;
        expectedRows = cfg.expectedRows;
    }
    else if (useFullTernaryL3)
    {
        makeFullTernaryL3(tables, parent, tableKeys, joinColInParent, joinColInChild,
                          randomRows, randomKeyRange, randomSeed);
        std::cout << "Dataset: full ternary tree L3"
                  << " rows/table=" << randomRows
                  << " key_range=" << randomKeyRange
                  << " seed=" << randomSeed << "\n";
    }
    else if (useStar15)
    {
        makeStarTree(15, tables, parent, tableKeys, joinColInParent, joinColInChild,
                     randomRows, randomKeyRange, randomSeed);
        std::cout << "Dataset: star-15 (root with 15 leaf children)"
                  << " rows/table=" << randomRows
                  << " key_range=" << randomKeyRange
                  << " seed=" << randomSeed << "\n";
    }
    else if (useStar4)
    {
        makeStarTree(4, tables, parent, tableKeys, joinColInParent, joinColInChild,
                     randomRows, randomKeyRange, randomSeed);
        std::cout << "Dataset: star-4 (root with 4 leaf children)"
                  << " rows/table=" << randomRows
                  << " key_range=" << randomKeyRange
                  << " seed=" << randomSeed << "\n";
    }
    else
    {
        tables = sampleTables();
        parent = {-1, 0, 0, 0, 3, 3};
        tableKeys = {3, 2, 2, 2, 2, 2};
        joinColInParent = {-1, 0, 1, 2, 0, 1};
        joinColInChild = {-1, 1, 0, 0, 0, 0};
    }

    FlatTables flat = flattenTables(tables);
    std::vector<int> result(maxCells, 0);
    sgx_enclave_id_t eid = createEnclave();

    if (!threadSweep.empty())
    {
        std::cout << "Thread sweep:";
        for (int t : threadSweep)
            std::cout << " " << t;
        std::cout << "\n";

        for (int threads : threadSweep)
        {
            std::cout << "\n=== threads=" << threads << " ===\n";
            std::vector<RunSummary> sweepSummaries;
            sweepSummaries.reserve(modes.size());
            int runTauOrOutSize = tauOrOutSize;
            for (int m : modes)
            {
                printModeBanner(modeName(m), threads);
                RunSummary s;
                s = runMode(eid, threads, m, runTauOrOutSize, flat, (int)tables.size(),
                            parent, root, joinColInParent, joinColInChild,
                            tableKeys, result, doEpsilon, doDelta, materializePadding, true, profile, stageProfile);
                s.paddingStatsOnly = isDOPaddingStatsOnly(s);
                sweepSummaries.push_back(s);
                if (s.callStatus != SGX_SUCCESS || s.ecallStatus != SGX_SUCCESS)
                {
                    std::fprintf(stderr, "%s failed: call=%#x (%s) ecall=%#x (%s)\n",
                                 modeName(m),
                                 s.callStatus, sgxStatusName(s.callStatus),
                                 s.ecallStatus, sgxStatusName(s.ecallStatus));
                    sgx_destroy_enclave(eid);
                    return 1;
                }
                std::cout << "  " << modeName(m) << ": ";
                if (s.skipped)
                    std::cout << "skipped";
                else
                    std::cout << s.ms << " ms";
                if ((profile || stageProfile) && !s.stageMs.empty() &&
                    (!s.paddingStatsOnly || s.rows > 0 || s.cols > 0))
                    std::cout << "  join-only=" << joinOnlyMs(s) << " ms";
                if (s.paddingStatsOnly && s.rows == 0 && s.cols == 0)
                    std::cout << "  stats-only";
                else if (s.paddingStatsOnly)
                    std::cout << "  protected-output-not-materialized rows=" << s.rows << " cols=" << s.cols;
                else
                    std::cout << "  rows=" << s.rows << " cols=" << s.cols;
                if (expectedRows >= 0)
                {
                    if (s.paddingStatsOnly && s.rows == 0 && s.cols == 0)
                        std::cout << "  stats-check=" << (stageMetric(s, 7) == expectedRows ? "OK" : "FAILED");
                    else
                        std::cout << "  check=" << (s.rows >= expectedRows ? "OK" : "FAILED");
                }
                std::cout << "\n";
                if ((profile || stageProfile) && !s.stageMs.empty())
                {
                    std::cout << "    stages:";
                    for (int i = 0; i < (int)s.stageMs.size(); ++i)
                    {
                        std::cout << " " << stageName(m, i) << "=";
                        if (stageIsMetric(i))
                            std::cout << (long long)s.stageMs[i];
                        else
                            std::cout << s.stageMs[i];
                    }
                    std::cout << "\n";
                    printDOPaddingSummary(s, "    ");
                }
                if (!materializePadding && expectedRows < 0 && runTauOrOutSize <= 0 && s.mode == ModeOurs)
                    runTauOrOutSize = s.rows;
            }

            if (runAll)
            {
                if (sweepSummaries.front().paddingStatsOnly &&
                    sweepSummaries.front().rows == 0 && sweepSummaries.front().cols == 0)
                {
                    std::cout << "  ratios: not measured (protected output stats only)\n";
                }
                else
                {
                    if (usedExactRowsInsteadOfProtectedRows(sweepSummaries.front()))
                        std::cout << "  DO protected output was too large to materialize; ratios use exact real output profiling.\n";
                    printComparisonSummary(sweepSummaries, "  ");
                }
            }
        }

        sgx_destroy_enclave(eid);
        return 0;
    }

    std::vector<RunSummary> summaries;
    summaries.reserve(modes.size());
    int runTauOrOutSize = tauOrOutSize;
    for (int m : modes)
    {
        printModeBanner(modeName(m), globalThreadCount);
        RunSummary s;
        s = runMode(eid, globalThreadCount, m, runTauOrOutSize, flat, (int)tables.size(),
                    parent, root, joinColInParent, joinColInChild,
                    tableKeys, result, doEpsilon, doDelta, materializePadding, benchmarkOnly, profile, stageProfile);
        s.paddingStatsOnly = isDOPaddingStatsOnly(s);
        summaries.push_back(s);
        if (s.callStatus != SGX_SUCCESS || s.ecallStatus != SGX_SUCCESS)
        {
            std::fprintf(stderr, "%s failed: call=%#x (%s) ecall=%#x (%s)\n",
                         modeName(m),
                         s.callStatus, sgxStatusName(s.callStatus),
                         s.ecallStatus, sgxStatusName(s.ecallStatus));
            sgx_destroy_enclave(eid);
            return 1;
        }

        std::cout << "Mode: " << modeName(m) << "\n";
        if (s.paddingStatsOnly && s.rows == 0 && s.cols == 0)
            std::cout << "Result: not materialized (DO padding stats only)\n";
        else if (s.paddingStatsOnly)
            std::cout << "Result: " << s.rows << " rows x " << s.cols
                      << " cols (DO protected output not materialized)\n";
        else
            std::cout << "Result: " << s.rows << " rows x " << s.cols << " cols\n";
        if (s.skipped)
            std::cout << "Profile ECALL time: skipped (same DO padding stats as first mode)\n";
        else
        {
            std::cout << (profile ? "Profile ECALL time: " : (stageProfile ? "Stage-profile ECALL time: " : (benchmarkOnly ? "Benchmark ECALL time: " : "ECALL time: "))) << s.ms << " ms\n";
            if ((profile || stageProfile) && !s.stageMs.empty())
                std::cout << "Join-only time: " << joinOnlyMs(s) << " ms\n";
        }
        if (expectedRows >= 0)
        {
            if (s.paddingStatsOnly && s.rows == 0 && s.cols == 0)
                std::cout << "Expected real rows: " << expectedRows << "  stats check="
                          << (stageMetric(s, 7) == expectedRows ? "OK" : "FAILED") << "\n";
            else if (s.paddingStatsOnly)
                std::cout << "Expected real rows: " << expectedRows << "  join check=" << (s.rows >= expectedRows ? "OK" : "FAILED") << "\n";
            else
                std::cout << "Expected real rows: " << expectedRows << "  protected check=" << (s.rows >= expectedRows ? "OK" : "FAILED") << "\n";
        }
        if ((profile || stageProfile) && !s.stageMs.empty())
        {
            std::cout << "Stage timings:\n";
            for (int i = 0; i < (int)s.stageMs.size(); ++i)
            {
                std::cout << "  " << stageName(m, i) << "=";
                if (stageIsMetric(i))
                    std::cout << (long long)s.stageMs[i] << "\n";
                else
                    std::cout << s.stageMs[i] << " ms\n";
            }
            printDOPaddingSummary(s, "");
        }
        if (!materializePadding && expectedRows < 0 && runTauOrOutSize <= 0 && s.mode == ModeOurs)
            runTauOrOutSize = s.rows;
    }

    if (runAll && !summaries.empty())
    {
        std::cout << "\nSummary:\n";
        if (summaries.front().paddingStatsOnly &&
            summaries.front().rows == 0 && summaries.front().cols == 0)
        {
            std::cout << "  DO padding stats only; protected output was too large to materialize.\n";
            std::cout << "  Algorithm runtimes and ratios were not measured.\n";
        }
        else
        {
            if (summaries.front().paddingStatsOnly)
                std::cout << "  DO protected output was too large to materialize; ratios use join-only unpadded profiling.\n";
            else if (usedExactRowsInsteadOfProtectedRows(summaries.front()))
                std::cout << "  DO protected output was too large to materialize; ratios use exact real output profiling.\n";
            printComparisonSummary(summaries, "  ");
        }
    }

    const RunSummary &last = summaries.back();

    if (benchmarkOnly || profile || stageProfile || printLimit <= 0)
    {
        sgx_destroy_enclave(eid);
        return 0;
    }

    Table output = arrayToTable(result.data(), last.rows, last.cols);
    int rowsToPrint = std::min(printLimit, last.rows);
    for (int r = 0; r < rowsToPrint; ++r)
    {
        const auto &row = output[r];
        std::cout << "  [";
        for (int i = 0; i < (int)row.size(); ++i)
        {
            if (i)
                std::cout << ", ";
            std::cout << row[i];
        }
        std::cout << "]\n";
    }
    if (last.rows > rowsToPrint)
        std::cout << "  ... (" << (last.rows - rowsToPrint) << " more rows)\n";

    sgx_destroy_enclave(eid);
    return 0;
}
