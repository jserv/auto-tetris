# auto-tetris

Play Tetris game automatically!

## Features
* AI for Tetris
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

## TODO
* Replace ncurses with direct terminal I/O. See [libtetris](https://github.com/HugoNikanor/libtetris) for tty graphics.
* Refine memory management. At present, leaks and buffer overrun exist.
* Colorize the blocks. [libtetris](https://github.com/HugoNikanor/libtetris) does the elegant work.
* Allow external programs to train the AI.

## License
`auto-tetris` is available under a permissive MIT-style license.
Use of this source code is governed by a MIT license that can be found in the [LICENSE](LICENSE) file.
