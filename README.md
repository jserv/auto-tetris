# auto-tetris
An advanced Tetris implementation with AI and human play modes, featuring seamless mode switching and classic gameplay mechanics.

## Features
1. Dual-mode gameplay: Watch AI play or take manual control with instant mode switching between AI and human modes during gameplay.
2. Classic Tetris: Full line clearing, scoring, and progressive difficulty with responsive controls and natural piece falling mechanics.
3. AI/Copilot: AI evaluates piece placement using height, gaps, and line completion potential with human-like timing and visible thinking pauses for watchable gameplay.
4. Terminal user interface: Clean, responsive TUI with real-time stats, next piece preview, line clearing animations, and color-coded pieces with preserved colors.
5. NES-Authentic Scoring: NES Tetris scoring system with level-based multipliers.

## Build and Run

### Prerequisites
* C compiler (GCC and Clang are tested)
* Unix-like terminal with ANSI escape sequence support
* GNU make

### Building
To build `auto-tetris`, run `make` inside the source directory:
```shell
$ make
```

### Running
To start the interactive game with TUI, run the executable:
```shell
$ ./tetris
```

**Controls:**

| Key | Action | Mode |
|-----|--------|------|
| Space | Toggle AI/Human mode | Both |
| P | Pause/unpause game | Both |
| Q | Quit game | Both |
| ↑ | Rotate piece | Human only |
| ← | Move piece left | Human only |
| → | Move piece right | Human only |
| ↓ | Drop piece to bottom | Human only |

**Gameplay Modes:**

Human Mode:
- Pieces automatically fall at timed intervals (speed increases with level)
- Use arrow keys for immediate piece control
- Classic Tetris feel with responsive controls

AI Mode:
- Computer calculates optimal piece placement
- Watch intelligent gameplay with strategic thinking delays
- AI considers multiple factors for piece positioning

**Scoring System:**

The game uses authentic NES Tetris scoring:
- Single (1 line): 40 × (level + 1) points
- Double (2 lines): 100 × (level + 1) points
- Triple (3 lines): 300 × (level + 1) points
- Tetris (4 lines): 1200 × (level + 1) points

### Benchmark Mode
To evaluate AI performance without the TUI:
```shell
# Single benchmark (1 game)
$ ./tetris -b

# Comprehensive benchmark (10 games)
$ ./tetris -b 10
```

**Benchmark Metrics:**
- Lines Cleared: Total lines cleared before game over
- Score: Final score achieved using standard Tetris scoring
- Pieces Placed: Number of tetromino pieces used
- LCPP: Lines Cleared Per Piece (efficiency metric)
- Game Duration: Time taken to complete the game
- Search Speed: AI decision-making speed (pieces/second)

### AI Weight Training
To evolve and optimize AI evaluation weights using genetic algorithms:
```shell
# Build training program
$ make train
# Basic training (default parameters)
$ ./train

# Custom training with 50 generations, 12 individuals, 5 games per evaluation
$ ./train -g 50 -p 12 -e 5

# Training with custom mutation rate and random seed
$ ./train -m 0.4 -s 12345
```

**Training Options:**

| Option | Description | Default |
|--------|-------------|---------|
| `-g N` | Maximum generations (-1 for infinite) | 100 |
| `-p N` | Population size (2-50) | 8 |
| `-e N` | Evaluation games per individual (1-20) | 3 |
| `-m RATE` | Mutation rate (0.0-1.0) | 0.3 |
| `-s SEED` | Random seed for reproducibility | time-based |
| `-h` | Show help and usage | - |

**Training Process:**
- Genetic Evolution: Uses tournament selection, crossover, and mutation
- Fitness Evaluation: Prioritizes lines-cleared-per-piece (LCPP) efficiency
- Progress Tracking: Real-time colored progress bars and fitness statistics
- Weight Export: Automatically saves evolved weights as C header files

## TODO
* Add customizable AI difficulty levels
* Create configuration file for key bindings

## License
`auto-tetris` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file.
