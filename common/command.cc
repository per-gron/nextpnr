/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *  Copyright (C) 2018  Miodrag Milanovic <miodrag@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef NO_GUI
#include <QApplication>
#include "application.h"
#include "mainwindow.h"
#endif
#ifndef NO_PYTHON
#include "pybindings.h"
#endif

#include <boost/filesystem/convenience.hpp>
#include <boost/program_options.hpp>
#include <fstream>
#include <iostream>
#include "command.h"
#include "design_utils.h"
#include "jsonparse.h"
#include "log.h"
#include "timing.h"
#include "version.h"

NEXTPNR_NAMESPACE_BEGIN

CommandHandler::CommandHandler(int argc, char **argv) : argc(argc), argv(argv) { log_files.push_back(stdout); }

bool CommandHandler::parseOptions()
{
    options.add(getGeneralOptions()).add(getArchOptions());
    try {
        po::parsed_options parsed =
                po::command_line_parser(argc, argv)
                        .style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing)
                        .options(options)
                        .positional(pos)
                        .run();
        po::store(parsed, vm);
        po::notify(vm);
        return true;
    } catch (std::exception &e) {
        std::cout << e.what() << "\n";
        return false;
    }
}

bool CommandHandler::executeBeforeContext()
{
    if (vm.count("help") || argc == 1) {
        std::cout << boost::filesystem::basename(argv[0])
                  << " -- Next Generation Place and Route (git sha1 " GIT_COMMIT_HASH_STR ")\n";
        std::cout << options << "\n";
        return argc != 1;
    }

    if (vm.count("version")) {
        std::cout << boost::filesystem::basename(argv[0])
                  << " -- Next Generation Place and Route (git sha1 " GIT_COMMIT_HASH_STR ")\n";
        return true;
    }
    validate();
    return false;
}

po::options_description CommandHandler::getGeneralOptions()
{
    po::options_description general("General options");
    general.add_options()("help,h", "show help");
    general.add_options()("verbose,v", "verbose output");
    general.add_options()("debug", "debug output");
    general.add_options()("force,f", "keep running after errors");
#ifndef NO_GUI
    general.add_options()("gui", "start gui");
#endif
#ifndef NO_PYTHON
    general.add_options()("run", po::value<std::vector<std::string>>(), "python file to execute");
    pos.add("run", -1);
#endif
    general.add_options()("json", po::value<std::string>(), "JSON design file to ingest");
    general.add_options()("seed", po::value<int>(), "seed value for random number generator");
    general.add_options()("slack_redist_iter", po::value<int>(), "number of iterations between slack redistribution");
    general.add_options()("cstrweight", po::value<float>(), "placer weighting for relative constraint satisfaction");
    general.add_options()("pack-only", "pack design only without placement or routing");

    general.add_options()("version,V", "show version");
    general.add_options()("test", "check architecture database integrity");
    general.add_options()("freq", po::value<double>(), "set target frequency for design in MHz");
    general.add_options()("no-tmdriv", "disable timing-driven placement");
    general.add_options()("save", po::value<std::string>(), "project file to write");
    general.add_options()("load", po::value<std::string>(), "project file to read");
    return general;
}

void CommandHandler::setupContext(Context *ctx)
{
    if (vm.count("verbose")) {
        ctx->verbose = true;
    }

    if (vm.count("debug")) {
        ctx->verbose = true;
        ctx->debug = true;
    }

    if (vm.count("force")) {
        ctx->force = true;
    }

    if (vm.count("seed")) {
        ctx->rngseed(vm["seed"].as<int>());
    }

    if (vm.count("slack_redist_iter")) {
        ctx->slack_redist_iter = vm["slack_redist_iter"].as<int>();
        if (vm.count("freq") && vm["freq"].as<double>() == 0) {
            ctx->auto_freq = true;
#ifndef NO_GUI
            if (!vm.count("gui"))
#endif
                log_warning("Target frequency not specified. Will optimise for max frequency.\n");
        }
    }

    if (vm.count("cstrweight")) {
        // ctx->placer_constraintWeight = vm["cstrweight"].as<float>();
    }

    if (vm.count("freq")) {
        auto freq = vm["freq"].as<double>();
        if (freq > 0)
            ctx->target_freq = freq * 1e6;
    }

    ctx->timing_driven = true;
    if (vm.count("no-tmdriv"))
        ctx->timing_driven = false;
}

int CommandHandler::executeMain(std::unique_ptr<Context> ctx)
{
    if (vm.count("test")) {
        ctx->archcheck();
        return 0;
    }

#ifndef NO_GUI
    if (vm.count("gui")) {
        Application a(argc, argv);
        MainWindow w(std::move(ctx), chipArgs);
        try {
            if (vm.count("json")) {
                std::string filename = vm["json"].as<std::string>();
                std::ifstream f(filename);
                if (!parse_json_file(f, filename, w.getContext()))
                    log_error("Loading design failed.\n");

                customAfterLoad(w.getContext());
                w.updateJsonLoaded();
            }
        } catch (log_execution_error_exception) {
            // show error is handled by gui itself
        }
        w.show();

        return a.exec();
    }
#endif
    if (vm.count("json")) {
        std::string filename = vm["json"].as<std::string>();
        std::ifstream f(filename);
        if (!parse_json_file(f, filename, ctx.get()))
            log_error("Loading design failed.\n");

        customAfterLoad(ctx.get());
    }

    if (vm.count("json") || vm.count("load")) {
        if (!ctx->pack() && !ctx->force)
            log_error("Packing design failed.\n");
        assign_budget(ctx.get());
        ctx->check();
        print_utilisation(ctx.get());
        if (!vm.count("pack-only")) {
            if (!ctx->place() && !ctx->force)
                log_error("Placing design failed.\n");
            ctx->check();
            if (!ctx->route() && !ctx->force)
                log_error("Routing design failed.\n");
        }

        customBitstream(ctx.get());
    }

#ifndef NO_PYTHON
    if (vm.count("run")) {
        init_python(argv[0], true);
        python_export_global("ctx", *ctx);

        std::vector<std::string> files = vm["run"].as<std::vector<std::string>>();
        for (auto filename : files)
            execute_python_file(filename.c_str());

        deinit_python();
    }
#endif

    if (vm.count("save")) {
        project.save(ctx.get(), vm["save"].as<std::string>());
    }

    return 0;
}

void CommandHandler::conflicting_options(const boost::program_options::variables_map &vm, const char *opt1,
                                         const char *opt2)
{
    if (vm.count(opt1) && !vm[opt1].defaulted() && vm.count(opt2) && !vm[opt2].defaulted()) {
        std::string msg = "Conflicting options '" + std::string(opt1) + "' and '" + std::string(opt2) + "'.";
        log_error("%s\n", msg.c_str());
    }
}

int CommandHandler::exec()
{
    try {
        if (!parseOptions())
            return -1;

        if (executeBeforeContext())
            return 0;

        std::unique_ptr<Context> ctx;
        if (vm.count("load")) {
            ctx = project.load(vm["load"].as<std::string>());
        } else {
            ctx = createContext();
        }
        setupContext(ctx.get());
        setupArchContext(ctx.get());
        return executeMain(std::move(ctx));
    } catch (log_execution_error_exception) {
        return -1;
    }
}

NEXTPNR_NAMESPACE_END
