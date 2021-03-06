/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <iostream>
#include <sstream>

#include <realm.hpp>
#include <realm/util/file.hpp>

#include "../util/timer.hpp"
#include "../util/random.hpp"
#include "../util/benchmark_results.hpp"
#include "../util/test_path.hpp"
#if REALM_ENABLE_ENCRYPTION
#include "../util/crypt_key.hpp"
#endif

using namespace realm;
using namespace realm::util;
using namespace realm::test_util;
using namespace realm::test_util::unit_test;

namespace {
#define BASE_SIZE 3600

/**
  This bechmark suite represents a number of common use cases,
  from the perspective of the bindings. It does *not* benchmark
  the type-safe C++ API, but only the things that language bindings
  are likely to use internally.

  This has the following implications:
  - All access is done with a SharedGroup in transactions.
  - The SharedGroup has full durability (is backed by a file).
    (but all benchmarks are also run with MemOnly durability for comparison)
  - Cases have been derived from:
    https://github.com/realm/realm-java/blob/bp-performance-test/realm/src/androidTest/java/io/realm/RealmPerformanceTest.java
*/

const size_t min_repetitions = 10;
const size_t max_repetitions = 1000;
const double min_duration_s = 0.1;
const double min_warmup_time_s = 0.05;

struct Benchmark {
    virtual ~Benchmark()
    {
    }
    virtual const char* name() const = 0;
    virtual void before_all(SharedGroup&)
    {
    }
    virtual void after_all(SharedGroup&)
    {
    }
    virtual void before_each(SharedGroup&)
    {
    }
    virtual void after_each(SharedGroup&)
    {
    }
    virtual void operator()(SharedGroup&) = 0;
};

struct BenchmarkUnorderedTableViewClear : Benchmark {
    const char* name() const
    {
        return "UnorderedTableViewClear";
    }

    void operator()(SharedGroup& group)
    {
        const size_t rows = 10000;
        WriteTransaction tr(group);
        TableRef tbl = tr.add_table(name());
        tbl->add_column(type_String, "s", true);
        tbl->add_empty_row(rows);

        tbl->add_search_index(0);

        for (size_t t = 0; t < rows / 3; t += 3) {
            tbl->set_string(0, t + 0, StringData("foo"));
            tbl->set_string(0, t + 1, StringData("bar"));
            tbl->set_string(0, t + 2, StringData("hello"));
        }

        TableView tv = (tbl->column<String>(0) == "foo").find_all();
        tv.clear();
    }
};

struct AddTable : Benchmark {
    const char* name() const
    {
        return "AddTable";
    }

    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef t = tr.add_table(name());
        t->add_column(type_String, "first");
        t->add_column(type_Int, "second");
        t->add_column(type_OldDateTime, "third");
        tr.commit();
    }

    void after_each(SharedGroup& group)
    {
        Group& g = group.begin_write();
        g.remove_table(name());
        group.commit();
    }
};

struct BenchmarkWithStringsTable : Benchmark {
    void before_all(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef t = tr.add_table("StringOnly");
        t->add_column(type_String, "chars");
        tr.commit();
    }

    void after_all(SharedGroup& group)
    {
        Group& g = group.begin_write();
        g.remove_table("StringOnly");
        group.commit();
    }
};

struct BenchmarkWithStrings : BenchmarkWithStringsTable {
    void before_all(SharedGroup& group)
    {
        BenchmarkWithStringsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        t->add_empty_row(BASE_SIZE * 4);
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            std::stringstream ss;
            ss << rand();
            auto s = ss.str();
            t->set_string(0, i, s);
        }
        tr.commit();
    }
};

struct BenchmarkWithStringsFewDup : BenchmarkWithStringsTable {
    void before_all(SharedGroup& group)
    {
        BenchmarkWithStringsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        t->add_empty_row(BASE_SIZE * 4);
        Random r;
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            std::stringstream ss;
            ss << r.draw_int(0, BASE_SIZE * 2);
            auto s = ss.str();
            t->set_string(0, i, s);
        }
        t->add_search_index(0);
        tr.commit();
    }
};

struct BenchmarkWithStringsManyDup : BenchmarkWithStringsTable {
    void before_all(SharedGroup& group)
    {
        BenchmarkWithStringsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        t->add_empty_row(BASE_SIZE * 4);
        Random r;
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            std::stringstream ss;
            ss << r.draw_int(0, 100);
            auto s = ss.str();
            t->set_string(0, i, s);
        }
        t->add_search_index(0);
        tr.commit();
    }
};

struct BenchmarkDistinctStringFewDupes : BenchmarkWithStringsFewDup {
    const char* name() const
    {
        return "DistinctStringFewDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->get_distinct_view(0);
    }
};

struct BenchmarkDistinctStringManyDupes : BenchmarkWithStringsManyDup {
    const char* name() const
    {
        return "DistinctStringManyDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->get_distinct_view(0);
    }
};

struct BenchmarkFindAllStringFewDupes : BenchmarkWithStringsFewDup {
    const char* name() const
    {
        return "FindAllStringFewDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->where().equal(0, StringData("10", 2)).find_all();
    }
};

struct BenchmarkFindAllStringManyDupes : BenchmarkWithStringsManyDup {
    const char* name() const
    {
        return "FindAllStringManyDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->where().equal(0, StringData("10", 2)).find_all();
    }
};

struct BenchmarkFindFirstStringFewDupes : BenchmarkWithStringsFewDup {
    const char* name() const
    {
        return "FindFirstStringFewDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        std::vector<std::string> strs = {
            "10", "20", "30", "40", "50", "60", "70", "80", "90", "100",
        };
        for (auto s : strs) {
            table->where().equal(0, StringData(s)).find();
        }
    }
};

struct BenchmarkFindFirstStringManyDupes : BenchmarkWithStringsManyDup {
    const char* name() const
    {
        return "FindFirstStringManyDupes";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        std::vector<std::string> strs = {
            "10", "20", "30", "40", "50", "60", "70", "80", "90", "100",
        };
        for (auto s : strs) {
            table->where().equal(0, StringData(s)).find();
        }
    }
};

struct BenchmarkWithLongStrings : BenchmarkWithStrings {
    void before_all(SharedGroup& group)
    {
        BenchmarkWithStrings::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        t->insert_empty_row(0);
        // This should be enough to upgrade the entire array:
        t->set_string(0, 0, "A really long string, longer than 63 bytes at least, I guess......");
        t->set_string(0, BASE_SIZE, "A really long string, longer than 63 bytes at least, I guess......");
        t->set_string(0, BASE_SIZE * 2, "A really long string, longer than 63 bytes at least, I guess......");
        t->set_string(0, BASE_SIZE * 3, "A really long string, longer than 63 bytes at least, I guess......");
        tr.commit();
    }
};

struct BenchmarkWithIntsTable : Benchmark {
    void before_all(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef t = tr.add_table("IntOnly");
        t->add_column(type_Int, "ints");
        tr.commit();
    }

    void after_all(SharedGroup& group)
    {
        Group& g = group.begin_write();
        g.remove_table("IntOnly");
        group.commit();
    }
};

struct BenchmarkWithInts : BenchmarkWithIntsTable {
    void before_all(SharedGroup& group)
    {
        BenchmarkWithIntsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("IntOnly");
        t->add_empty_row(BASE_SIZE * 4);
        Random r;
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            t->set_int(0, i, r.draw_int<int64_t>());
        }
        tr.commit();
    }
};

struct BenchmarkQuery : BenchmarkWithStrings {
    const char* name() const
    {
        return "Query";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->find_all_string(0, "200");
    }
};

struct BenchmarkSize : BenchmarkWithStrings {
    const char* name() const
    {
        return "Size";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        volatile size_t dummy = table->size();
        static_cast<void>(dummy);
    }
};

struct BenchmarkSort : BenchmarkWithStrings {
    const char* name() const
    {
        return "Sort";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        ConstTableView view = table->get_sorted_view(0);
    }
};

struct BenchmarkEmptyCommit : Benchmark {
    const char* name() const
    {
        return "EmptyCommit";
    }

    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        tr.commit();
    }
};

struct BenchmarkSortInt : BenchmarkWithInts {
    const char* name() const
    {
        return "SortInt";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("IntOnly");
        ConstTableView view = table->get_sorted_view(0);
    }
};

struct BenchmarkDistinctIntFewDupes : BenchmarkWithIntsTable {
    const char* name() const
    {
        return "DistinctIntNoDupes";
    }

    void before_all(SharedGroup& group)
    {
        BenchmarkWithIntsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("IntOnly");
        t->add_empty_row(BASE_SIZE * 4);
        Random r;
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            t->set_int(0, i, r.draw_int(0, BASE_SIZE * 2));
        }
        tr.commit();
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("IntOnly");
        ConstTableView view = table->where().find_all();
        view.distinct(0);
    }
};

struct BenchmarkDistinctIntManyDupes : BenchmarkWithIntsTable {
    const char* name() const
    {
        return "DistinctIntManyDupes";
    }

    void before_all(SharedGroup& group)
    {
        BenchmarkWithIntsTable::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("IntOnly");
        t->add_empty_row(BASE_SIZE * 4);
        Random r;
        for (size_t i = 0; i < BASE_SIZE * 4; ++i) {
            t->set_int(0, i, r.draw_int(0, 10));
        }
        tr.commit();
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("IntOnly");
        ConstTableView view = table->where().find_all();
        view.distinct(0);
    }
};

struct BenchmarkInsert : BenchmarkWithStringsTable {
    const char* name() const
    {
        return "Insert";
    }

    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        for (size_t i = 0; i < 10000; ++i) {
            t->add_empty_row();
            t->set_string(0, i, "a");
        }
        tr.commit();
    }
};

struct BenchmarkGetString : BenchmarkWithStrings {
    const char* name() const
    {
        return "GetString";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        size_t len = table->size();
        volatile int dummy = 0;
        for (size_t i = 0; i < len; ++i) {
            StringData str = table->get_string(0, i);
            dummy += str[0]; // to avoid over-optimization
        }
    }
};

struct BenchmarkSetString : BenchmarkWithStrings {
    const char* name() const
    {
        return "SetString";
    }

    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef table = tr.get_table("StringOnly");
        size_t len = table->size();
        for (size_t i = 0; i < len; ++i) {
            table->set_string(0, i, "c");
        }
        tr.commit();
    }
};

struct BenchmarkCreateIndex : BenchmarkWithStrings {
    const char* name() const
    {
        return "CreateIndex";
    }
    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef table = tr.get_table("StringOnly");
        table->add_search_index(0);
        tr.commit();
    }
};

struct BenchmarkGetLongString : BenchmarkWithLongStrings {
    const char* name() const
    {
        return "GetLongString";
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        size_t len = table->size();
        volatile int dummy = 0;
        for (size_t i = 0; i < len; ++i) {
            StringData str = table->get_string(0, i);
            dummy += str[0]; // to avoid over-optimization
        }
    }
};

struct BenchmarkQueryLongString : BenchmarkWithStrings {
    static constexpr const char* long_string = "This is some other long string, that takes a lot of time to find";
    bool ok;

    const char* name() const
    {
        return "QueryLongString";
    }

    void before_all(SharedGroup& group)
    {
        BenchmarkWithStrings::before_all(group);
        WriteTransaction tr(group);
        TableRef t = tr.get_table("StringOnly");
        t->set_string(0, 0, "Some random string");
        t->set_string(0, 1, long_string);
        tr.commit();
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table("StringOnly");
        StringData str(long_string);
        ok = true;
        auto q = table->where().equal(0, str);
        for (size_t ndx = 0; ndx < 1000; ndx++) {
            auto res = q.find();
            if (res != 1) {
                ok = false;
            }
        }
    }
};

struct BenchmarkSetLongString : BenchmarkWithLongStrings {
    const char* name() const
    {
        return "SetLongString";
    }

    void operator()(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef table = tr.get_table("StringOnly");
        size_t len = table->size();
        for (size_t i = 0; i < len; ++i) {
            table->set_string(0, i, "c");
        }
        tr.commit();
    }
};

struct BenchmarkQueryNot : Benchmark {
    const char* name() const
    {
        return "QueryNot";
    }

    void before_all(SharedGroup& group)
    {
        WriteTransaction tr(group);
        TableRef table = tr.add_table(name());
        table->add_column(type_Int, "first");
        table->add_empty_row(1000);
        for (size_t i = 0; i < 1000; ++i) {
            table->set_int(0, i, 1);
        }
        tr.commit();
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table(name());
        Query q = table->where();
        q.not_equal(0, 2); // never found, = worst case
        TableView results = q.find_all();
        results.size();
    }

    void after_all(SharedGroup& group)
    {
        Group& g = group.begin_write();
        g.remove_table(name());
        group.commit();
    }
};

struct BenchmarkGetLinkList : Benchmark {
    const char* name() const
    {
        return "GetLinkList";
    }
    static const size_t rows = 10000;

    void before_all(SharedGroup& group)
    {
        WriteTransaction tr(group);
        std::string n = std::string(name()) + "_Destination";
        TableRef destination_table = tr.add_table(n);
        TableRef table = tr.add_table(name());
        table->add_column_link(type_LinkList, "linklist", *destination_table);
        table->add_empty_row(rows);
        tr.commit();
    }

    void operator()(SharedGroup& group)
    {
        ReadTransaction tr(group);
        ConstTableRef table = tr.get_table(name());
        std::vector<ConstLinkViewRef> linklists(rows);
        for (size_t i = 0; i < rows; ++i) {
            linklists[i] = table->get_linklist(0, i);
        }
        for (size_t i = 0; i < rows; ++i) {
            table->get_linklist(0, i);
        }
        for (size_t i = 0; i < rows; ++i) {
            linklists[i].reset();
        }
    }

    void after_all(SharedGroup& group)
    {
        Group& g = group.begin_write();
        g.remove_table(name());
        auto n = std::string(name()) + "_Destination";
        g.remove_table(n);
        group.commit();
    }
};

const char* to_lead_cstr(SharedGroupOptions::Durability level)
{
    switch (level) {
        case SharedGroupOptions::Durability::Full:
            return "Full   ";
        case SharedGroupOptions::Durability::MemOnly:
            return "MemOnly";
#ifndef _WIN32
        case SharedGroupOptions::Durability::Async:
            return "Async  ";
#endif
    }
    return nullptr;
}

const char* to_ident_cstr(SharedGroupOptions::Durability level)
{
    switch (level) {
        case SharedGroupOptions::Durability::Full:
            return "Full";
        case SharedGroupOptions::Durability::MemOnly:
            return "MemOnly";
#ifndef _WIN32
        case SharedGroupOptions::Durability::Async:
            return "Async";
#endif
    }
    return nullptr;
}

void run_benchmark_once(Benchmark& benchmark, SharedGroup& sg, Timer& timer)
{
    timer.pause();
    benchmark.before_each(sg);
    timer.unpause();

    benchmark(sg);

    timer.pause();
    benchmark.after_each(sg);
    timer.unpause();
}

/// This little piece of likely over-engineering runs the benchmark a number of times,
/// with each durability setting, and reports the results for each run.
template <typename B>
void run_benchmark(TestContext& test_context, BenchmarkResults& results)
{
    typedef std::pair<SharedGroupOptions::Durability, const char*> config_pair;
    std::vector<config_pair> configs;

    configs.push_back(config_pair(SharedGroupOptions::Durability::MemOnly, nullptr));
#if REALM_ENABLE_ENCRYPTION
    configs.push_back(config_pair(SharedGroupOptions::Durability::MemOnly, crypt_key(true)));
#endif

    configs.push_back(config_pair(SharedGroupOptions::Durability::Full, nullptr));

#if REALM_ENABLE_ENCRYPTION
    configs.push_back(config_pair(SharedGroupOptions::Durability::Full, crypt_key(true)));
#endif

    Timer timer(Timer::type_UserTime);

    for (auto it = configs.begin(); it != configs.end(); ++it) {
        SharedGroupOptions::Durability level = it->first;
        const char* key = it->second;
        B benchmark;

        // Generate the benchmark result texts:
        std::stringstream lead_text_ss;
        std::stringstream ident_ss;
        lead_text_ss << benchmark.name() << " (" << to_lead_cstr(level) << ", "
                     << (key == nullptr ? "EncryptionOff" : "EncryptionOn") << ")";
        ident_ss << benchmark.name() << "_" << to_ident_cstr(level)
                 << (key == nullptr ? "_EncryptionOff" : "_EncryptionOn");
        std::string ident = ident_ss.str();

        // Open a SharedGroup:
        SHARED_GROUP_TEST_PATH(realm_path);
        std::unique_ptr<SharedGroup> group;
        group.reset(new SharedGroup(realm_path, false, SharedGroupOptions(level, key)));

        benchmark.before_all(*group);

        // Warm-up and initial measuring:
        size_t num_warmup_reps = 1;
        double time_to_execute_warmup_reps = 0;
        while (time_to_execute_warmup_reps < min_warmup_time_s && num_warmup_reps < max_repetitions) {
            num_warmup_reps *= 10;
            Timer t(Timer::type_UserTime);
            for (size_t i = 0; i < num_warmup_reps; ++i) {
                run_benchmark_once(benchmark, *group, t);
            }
            time_to_execute_warmup_reps = t.get_elapsed_time();
        }

        size_t required_reps = size_t(min_duration_s / (time_to_execute_warmup_reps / num_warmup_reps));
        if (required_reps < min_repetitions) {
            required_reps = min_repetitions;
        }
        if (required_reps > max_repetitions) {
            required_reps = max_repetitions;
        }

        for (size_t rep = 0; rep < required_reps; ++rep) {
            Timer t;
            run_benchmark_once(benchmark, *group, t);
            double s = t.get_elapsed_time();
            results.submit(ident.c_str(), s);
        }

        benchmark.after_all(*group);

        results.finish(ident, lead_text_ss.str());
    }
    std::cout << std::endl;
}

} // anonymous namespace

extern "C" int benchmark_common_tasks_main();

TEST(benchmark_common_tasks_main)
{
    std::string results_file_stem = test_util::get_test_path_prefix() + "results";
    BenchmarkResults results(40, results_file_stem.c_str());

#define BENCH(B) run_benchmark<B>(test_context, results)

    BENCH(BenchmarkUnorderedTableViewClear);
    BENCH(BenchmarkEmptyCommit);
    BENCH(AddTable);
    BENCH(BenchmarkQuery);
    BENCH(BenchmarkQueryNot);
    BENCH(BenchmarkSize);
    BENCH(BenchmarkSort);
    BENCH(BenchmarkSortInt);
    BENCH(BenchmarkDistinctIntFewDupes);
    BENCH(BenchmarkDistinctIntManyDupes);
    BENCH(BenchmarkDistinctStringFewDupes);
    BENCH(BenchmarkDistinctStringManyDupes);
    BENCH(BenchmarkFindAllStringFewDupes);
    BENCH(BenchmarkFindAllStringManyDupes);
    BENCH(BenchmarkFindFirstStringFewDupes);
    BENCH(BenchmarkFindFirstStringManyDupes);
    BENCH(BenchmarkInsert);
    BENCH(BenchmarkGetString);
    BENCH(BenchmarkSetString);
    BENCH(BenchmarkCreateIndex);
    BENCH(BenchmarkGetLongString);
    BENCH(BenchmarkQueryLongString);
    BENCH(BenchmarkSetLongString);
    BENCH(BenchmarkGetLinkList);

#undef BENCH
}

#if !REALM_IOS
int main(int, const char**)
{
    bool success;

    success = get_default_test_list().run();

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
