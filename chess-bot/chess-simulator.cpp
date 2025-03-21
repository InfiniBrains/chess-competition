#include "chess-simulator.h"
// disservin's lib. drop a star on his hard work!
// https://github.com/Disservin/chess-library
#include <cstdio>
#include <string>
#include <array>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <random>
#include <thread>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

using namespace ChessSimulator;

// Better stockfish communication using proper process management
std::string run_stockfish(const std::string& fen) {
    // Try multiple potential paths for stockfish
    const std::array<std::string, 4> stockfish_paths = {
        "/usr/local/bin/stockfish",
        "/app/stockfish",
        "stockfish",  // Try using PATH environment
        "/opt/homebrew/bin/stockfish"
    };

    std::string stockfish_path;
    for (const auto& path : stockfish_paths) {
        if (access(path.c_str(), X_OK) == 0) {
            stockfish_path = path;
            break;
        }
    }

    if (stockfish_path.empty()) {
        throw std::runtime_error("Could not find stockfish executable in any of the expected paths.");
    }

    // Create pipes for communication
    int parent_to_child[2]; // For writing to stockfish
    int child_to_parent[2]; // For reading from stockfish

    if (pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0) {
        throw std::runtime_error("Failed to create pipes");
    }

    // Fork a child process
    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        throw std::runtime_error("Failed to fork process");
    } 
    else if (pid == 0) {
        // Child process - will become stockfish
        
        // Redirect stdin to read from parent
        close(parent_to_child[1]); // Close unused write end
        dup2(parent_to_child[0], STDIN_FILENO);
        close(parent_to_child[0]);
        
        // Redirect stdout to write to parent
        close(child_to_parent[0]); // Close unused read end
        dup2(child_to_parent[1], STDOUT_FILENO);
        close(child_to_parent[1]);
        
        // Execute stockfish
        execl(stockfish_path.c_str(), stockfish_path.c_str(), (char*)NULL);
        
        // If we get here, exec failed
        perror("exec failed");
        exit(EXIT_FAILURE);
    }
    
    // Parent process
    
    // Close unused pipe ends
    close(parent_to_child[0]);
    close(child_to_parent[1]);
    
    // Make read end non-blocking
    int flags = fcntl(child_to_parent[0], F_GETFL, 0);
    fcntl(child_to_parent[0], F_SETFL, flags | O_NONBLOCK);
    
    // Communicate with stockfish
    FILE* to_engine = fdopen(parent_to_child[1], "w");
    FILE* from_engine = fdopen(child_to_parent[0], "r");
    
    // Send UCI commands
    fprintf(to_engine, "uci\n");
    fflush(to_engine);
    
    // Sleep to give engine time to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Configure engine
    fprintf(to_engine, "setoption name Threads value %d\n", std::thread::hardware_concurrency());
    fprintf(to_engine, "setoption name Hash value 512\n"); // Increased to 512MB
    fprintf(to_engine, "setoption name Skill Level value 20\n");
    fprintf(to_engine, "setoption name MultiPV value 1\n");
    fprintf(to_engine, "isready\n");
    fflush(to_engine);
    
    // Read until "readyok"
    char buffer[4096];
    std::string result;
    auto start_time = std::chrono::steady_clock::now();
    bool ready = false;
    
    while (!ready) {
        if (fgets(buffer, sizeof(buffer), from_engine)) {
            result += buffer;
            if (strstr(buffer, "readyok")) {
                ready = true;
            }
        }
        
        // Check for timeout
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() > 5) {
            break;
        }
        
        // Don't burn CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Send position command
    fprintf(to_engine, "position fen %s\n", fen.c_str());
    fflush(to_engine);
    

    fprintf(to_engine, "go movetime 1000\n");
    fflush(to_engine);
    
    // Collect output until bestmove appears
    result.clear();
    start_time = std::chrono::steady_clock::now();
    bool found_bestmove = false;
    
    while (!found_bestmove) {
        // Try to read from engine
        if (fgets(buffer, sizeof(buffer), from_engine)) {
            result += buffer;
            if (strstr(buffer, "bestmove")) {
                found_bestmove = true;
            }
        }
        
        // Check for timeout (7 seconds max wait)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() > 7) {
            break;
        }
        
        // Don't burn CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Clean up
    fprintf(to_engine, "quit\n");
    fflush(to_engine);
    
    // Close pipes
    fclose(to_engine);
    fclose(from_engine);
    
    // Kill child process if it's still running
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    
    return result;
}

std::string ChessSimulator::Move(std::string fen) {
    try {
        // Run stockfish with the FEN position
        std::string output = run_stockfish(fen);
        
        // Debug info - uncomment if needed
        // std::cerr << "Stockfish output length: " << output.size() << " bytes" << std::endl;
        
        // Parse the output to find the "bestmove" line
        size_t bestmovePos = output.find("bestmove");
        if (bestmovePos != std::string::npos) {
            // Extract the move (typically "bestmove e2e4 ponder d7d5")
            std::string line = output.substr(bestmovePos);
            
            // Parse just the move part (first 4-5 characters after "bestmove ")
            size_t spacePos = line.find(" ", 9); // Find first space after "bestmove "
            if (spacePos == std::string::npos) {
                spacePos = line.length();
            }
            
            std::string bestMove = line.substr(9, spacePos - 9);
            return bestMove;
        } else {
            std::cerr << "Error: 'bestmove' not found in stockfish output" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    return "";
}