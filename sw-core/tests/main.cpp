// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include <iostream>

void run_content_source_tests();
void run_cache_store_tests();
void run_pipeline_integration_tests();

int main() {
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
