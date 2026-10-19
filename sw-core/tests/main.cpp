// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include <iostream>

#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

void run_content_source_tests();
void run_cache_store_tests();
void run_pipeline_integration_tests();

// Os testes verificam com SW_CHECK (check.hpp), que não some com NDEBUG. Ainda assim, no MSVC
// em Debug, avisos do CRT/STL abrem uma caixa de diálogo e esperam alguém clicar:
// no CI o teste ficaria pendurado em vez de falhar. Mandamos a mensagem para o
// stderr e abortamos com código de erro.
static void make_assert_fail_instead_of_hang()
{
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
}

int main()
{
    make_assert_fail_instead_of_hang();
    std::cout << "========================================\n";
    std::cout << " Running Swarmstream sw-core Unit & Integration Tests\n";
    std::cout << "========================================\n\n";

    try {
        std::cout << "--- Content Source Tests ---\n";
        run_content_source_tests();
        std::cout << "-> Content Source tests passed.\n\n";

        std::cout << "--- Cache Store Tests ---\n";
        run_cache_store_tests();
        std::cout << "-> Cache Store tests passed.\n\n";

        std::cout << "--- Pipeline Integration Tests (F0-07) ---\n";
        run_pipeline_integration_tests();
        std::cout << "-> Pipeline Integration tests passed.\n\n";

        std::cout << "========================================\n";
        std::cout << " ALL TESTS PASSED SUCCESSFULLY! (100%)\n";
        std::cout << "========================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with unhandled exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Test failed with unknown exception.\n";
        return 1;
    }
}
