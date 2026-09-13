#include "cli/driver.h"
#include "cli/run.h"

int main(int argc, char** argv) {
    bronze::cli::registerRunHooks(bronze::cli::runEvalReal, bronze::cli::runFileInJitReal);
    return bronze::cli::runDriver(argc, argv);
}
