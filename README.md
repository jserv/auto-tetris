# auto-tetris
An advanced Tetris implementation with AI and human play modes, featuring seamless mode switching and classic gameplay mechanics.

## Features
- Dual-mode gameplay: Seamless switching between AI and human control during gameplay
- Classic Tetris mechanics: Line clearing, NES-authentic scoring, progressive difficulty
- Intelligent AI: Multi-factor piece placement evaluation with human-like timing
- Terminal UI: Color-coded pieces, real-time stats, animations, next piece preview

## Build and Run

### Quick Start
Prerequisites: C compiler, GNU make, ANSI terminal support

```shell
$ make        # Build
$ ./tetris    # Run interactive game
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

Game Modes:
- Human: Classic controls with progressive speed increases
- AI: Optimal piece placement with strategic thinking delays

NES Scoring: 1-line: 40×(level+1), 2-line: 100×(level+1), 3-line: 300×(level+1), Tetris: 1200×(level+1)

### Benchmark Mode
```shell
$ ./tetris -b      # Single game
$ ./tetris -b 10   # Multi-game analysis
```

**Benchmark Metrics:**
- Lines Cleared: Total lines cleared before game over
- Score: Final score achieved using standard Tetris scoring
- Pieces Placed: Number of tetromino pieces used
- LCPP: Lines Cleared Per Piece (efficiency metric)
- Game Duration: Time taken to complete the game
- Search Speed: AI decision-making speed (pieces/second)

### AI Training
```shell
$ make train && ./train    # Basic genetic algorithm training
$ ./train -g 50 -p 12 -e 5 # Custom: 50 generations, 12 population, 5 eval games
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
