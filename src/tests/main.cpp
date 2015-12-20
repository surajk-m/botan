/*
* (C) 2015 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include "../cli/cli.h"
#include "tests.h"
#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <deque>
#include <thread>
#include <future>

#include <botan/version.h>
#include <botan/auto_rng.h>
#include <botan/loadstor.h>

#if defined(BOTAN_HAS_HMAC_DRBG)
#include <botan/hmac_drbg.h>
#endif

#if defined(BOTAN_HAS_SYSTEM_RNG)
#include <botan/system_rng.h>
#endif

namespace {

using Botan_Tests::Test;

std::unique_ptr<Botan::RandomNumberGenerator>
setup_tests(std::ostream& out, size_t threads,
            size_t soak_level,
            bool log_success,
            std::string drbg_seed)
   {
   out << "Testing " << Botan::version_string() << "\n";
   out << "Starting tests";

   if(threads > 1)
      out << " threads:" << threads;

   out << " soak level:" << soak_level;

   std::unique_ptr<Botan::RandomNumberGenerator> rng;

#if defined(BOTAN_HAS_HMAC_DRBG)
   if(drbg_seed == "")
      {
      const uint64_t ts = Test::timestamp();
      std::vector<uint8_t> ts8(8);
      Botan::store_be(ts, ts8.data());
      drbg_seed = Botan::hex_encode(ts8);
      }

   out << " rng:HMAC_DRBG with seed '" << drbg_seed << "'";
   rng.reset(new Botan::Serialized_RNG(new Botan::HMAC_DRBG("HMAC(SHA-384)")));
   const std::vector<uint8_t> seed = Botan::hex_decode(drbg_seed);
   rng->add_entropy(seed.data(), seed.size());

#else

   if(drbg_seed != "")
      throw Botan_Tests::Test_Error("HMAC_DRBG disabled in build, cannot specify DRBG seed");

#if defined(BOTAN_HAS_SYSTEM_RNG)
   out << " rng:system";
   rng.reset(new Botan::System_RNG);
#else
   // AutoSeeded_RNG always available
   out << " rng:autoseeded";
   rng.reset(new Botan::Serialized_RNG(new Botan::AutoSeeded_RNG));
#endif

#endif

   out << std::endl;

   Botan_Tests::Test::setup_tests(soak_level, log_success, rng.get());

   return rng;
   }

class Test_Runner : public Botan_CLI::Command
   {
   public:
      Test_Runner() : Command("test --threads=0 --soak=5 --drbg-seed= --log-success *suites") {}

      std::string help_text() const override
         {
         std::ostringstream err;

         err << "Usage: botan-test [--drbg-seed=] [--threads=N] [--log-success] "
             << "suite suite ...\n\n"
             << "Available suites\n"
             << "----------------\n";

         size_t line_len = 0;

         for(auto&& test : Test::registered_tests())
            {
            err << test << " ";
            line_len += test.size() + 1;

            if(line_len > 64)
               {
               err << "\n";
               line_len = 0;
               }
            }

         if(line_len > 0)
            {
            err << "\n";
            }

         return err.str();
         }

      void go() override
         {
         const size_t threads = get_arg_sz("threads");
         const size_t soak = get_arg_sz("soak");
         const std::string drbg_seed = get_arg("drbg-seed");
         bool log_success = flag_set("log-success");

         std::vector<std::string> req = get_arg_list("suites");

         if(req.empty())
            {
            /*
            If nothing was requested on the command line, run everything. First
            run the "essentials" to smoke test, then everything else in
            alphabetical order.
            */
            req = {"block", "stream", "hash", "mac", "modes", "aead"
                   "kdf", "pbkdf", "hmac_drbg", "x931_rng", "util"};

            std::set<std::string> all_others = Botan_Tests::Test::registered_tests();

            for(auto f : req)
               {
               all_others.erase(f);
               }

            req.insert(req.end(), all_others.begin(), all_others.end());
            }

         std::unique_ptr<Botan::RandomNumberGenerator> rng =
            setup_tests(std::cout, threads, soak, log_success, drbg_seed);

         size_t failed = run_tests(req, std::cout, threads);

         // Throw so main returns an error
         if(failed)
            throw Botan_Tests::Test_Error("Test suite failure");
         }
   private:

      std::string report_out(const std::vector<Test::Result>& results,
                             size_t& tests_failed,
                             size_t& tests_ran)
         {
         std::ostringstream out;

         std::map<std::string, Test::Result> combined;
         for(auto&& result : results)
            {
            const std::string who = result.who();
            auto i = combined.find(who);
            if(i == combined.end())
               {
               combined[who] = Test::Result(who);
               i = combined.find(who);
               }

            i->second.merge(result);
            }

         for(auto&& result : combined)
            {
            out << result.second.result_string(verbose());
            tests_failed += result.second.tests_failed();
            tests_ran += result.second.tests_run();
            }

         return out.str();
         }


      size_t run_tests(const std::vector<std::string>& tests_to_run,
                       std::ostream& out,
                       size_t threads)
         {
         size_t tests_ran = 0, tests_failed = 0;

         if(threads <= 1)
            {
            for(auto&& test_name : tests_to_run)
               {
               std::vector<Test::Result> results = Test::run_test(test_name, false);
               out << report_out(results, tests_failed, tests_ran) << std::flush;
               }
            }
         else
            {

            /*
            We're not doing this in a particularly nice way, and variance in time is
            high so commonly we'll 'run dry' by blocking on the first future. But
            plain C++11 <thread> is missing a lot of tools we'd need (like
            wait_for_any on a set of futures) and there is no point pulling in an
            additional dependency just for this. In any case it helps somewhat
            (50-100% speedup) and provides a proof of concept for parallel testing.
            */

            typedef std::future<std::vector<Test::Result>> FutureResults;
            std::deque<FutureResults> fut_results;

            for(auto&& test_name : tests_to_run)
               {
               fut_results.push_back(std::async(std::launch::async,
                                                [test_name]() { return Test::run_test(test_name, false); }));

               while(fut_results.size() > threads)
                  {
                  out << report_out(fut_results[0].get(), tests_failed, tests_ran) << std::flush;
                  fut_results.pop_front();
                  }
               }

            while(fut_results.size() > 0)
               {
               out << report_out(fut_results[0].get(), tests_failed, tests_ran) << std::flush;
               fut_results.pop_front();
               }
            }

         out << "Tests complete ran " << tests_ran << " tests ";

         if(tests_failed > 0)
            {
            out << tests_failed << " tests failed";
            }
         else if(tests_ran > 0)
            {
            out << "all tests ok";
            }

         out << std::endl;

         return tests_failed;
         }


   };

BOTAN_REGISTER_COMMAND(Test_Runner);

}

int main(int argc, char* argv[])
   {
   std::cerr << Botan::runtime_version_check(BOTAN_VERSION_MAJOR,
                                             BOTAN_VERSION_MINOR,
                                             BOTAN_VERSION_PATCH);

   Botan_CLI::Command* cmd = Botan_CLI::Command::get_cmd("test");

   if(!cmd)
      {
      std::cout << "Unable to retrieve testing helper (program bug)\n"; // WTF
      return 1;
      }

   std::vector<std::string> args(argv + 1, argv + argc);
   return cmd->run(args);
   }
