# auto-tetris
An advanced Tetris implementation with AI and human play modes, featuring seamless mode switching and classic gameplay mechanics.

## Features
1. Dual-mode gameplay: Watch intelligent AI play or take manual control with instant mode switching between AI and human modes during gameplay.
2. Classic Tetris: Full line clearing, scoring, and progressive difficulty with responsive controls and natural piece falling mechanics.
3. AI/Copilot: AI evaluates piece placement using height, gaps, and line completion potential with human-like timing and visible thinking pauses for watchable gameplay.
4. Terminal user interface: Clean, responsive TUI with real-time stats, next piece preview, line clearing animations, and color-coded pieces with preserved colors.
5. Scoring: 100 points per line, bonus multipliers for simultaneous clears, level progression every 10 lines with increasing speed.

## Build and Run

### Requirements
* C compiler (GCC and Clang are tested)
* Unix-like terminal with ANSI escape sequence support
* GNU make

### Building
To build `auto-tetris`, run `make` inside the source directory:
```shell
$ make
```

### Running
To start the game, run the executable:
```shell
$ ./tetris
```

## Controls

| Key | Action | Mode |
|-----|--------|------|
| Space | Toggle AI/Human mode | Both |
| P | Pause/unpause game | Both |
| Q | Quit game | Both |
| ↑ | Rotate piece | Human only |
| ← | Move piece left | Human only |
| → | Move piece right | Human only |
| ↓ | Drop piece to bottom | Human only |

### Gameplay Modes

Human Mode:
- Pieces automatically fall at timed intervals (speed increases with level)
- Use arrow keys for immediate piece control
- Classic Tetris feel with responsive controls

AI Mode:  
- Computer calculates optimal piece placement
- Watch intelligent gameplay with strategic thinking delays
- AI considers multiple factors for piece positioning

## TODO
* Allow external programs to train the AI weights
* Add customizable AI difficulty levels
* Implement save/load high scores functionality
* Add network multiplayer support
* Create configuration file for key bindings

## License
`auto-tetris` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file.
