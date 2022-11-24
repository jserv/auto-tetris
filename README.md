# auto-tetris

Play Tetris game automatically!

## Features
* AI for Tetris
    - DFS to a certain depth
    - Heuristic used to evaluate leaves
* Simple ncurses based user interface

## Build and Run
`auto-tetris` requires [ncurses](https://invisible-island.net/ncurses/).
Debian/Ubuntu Dependencies:
```shell
sudo apt install libncurses5-dev
```

To build `auto-tetris`, run `make` inside the directory where you have the source.
```shell
make
```

To play the game, run the executable 'tetris'.
```shell
./tetris
```

Key mapping:
* space: Pause toggle
* Q: Quit the game

## License
`auto-tetris` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file.
