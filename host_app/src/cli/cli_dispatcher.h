#pragma once

class CliDispatcher {
public:
    static bool should_run_cli(int argc, wchar_t **argv);
    static int dispatch(int argc, wchar_t **argv);
};
