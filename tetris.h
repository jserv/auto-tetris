#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { BOT, LEFT, TOP, RIGHT } direction_t;

typedef struct {
    uint8_t x, y;
} coord_t;

/* The max len of any blocks read at runtime */
#define MAX_BLOCK_LEN 4

/* Number of standard Tetris shapes (tetrominoes) */
#define NUM_TETRIS_SHAPES 7

typedef struct {
    int n_rot;
    coord_t rot_wh[4];
    int **crust[4][4];
    int crust_len[4][4];
    int crust_flat[4][4][MAX_BLOCK_LEN][2];  // direction, rotation, block, rc
    int max_dim_len;
    int **rot[4];
    int rot_flat[4][MAX_BLOCK_LEN][2]; /* rotation, block, rc */
} shape_t;

bool shapes_init(void);

typedef struct {
    coord_t offset;
    int rot;
    shape_t *shape;
} block_t;

block_t *block_new(void);
void block_init(block_t *b, shape_t *s);
void block_get(block_t *b, int i, coord_t *result);
void block_rotate(block_t *b, int amount);
void block_move(block_t *b, direction_t d, int amount);
int block_extreme(const block_t *b, direction_t d);

typedef struct {
    shape_t *shape;
    int rot;
    int col;
} move_t;

typedef struct {
    bool **rows;
    int **stacks;
    int *stack_cnt;
    int *relief;
    int *n_row_fill;
    int *full_rows;
    int n_full_rows;
    int width, height;

    int n_total_cleared, n_last_cleared;
    int *gaps;
} grid_t;

grid_t *grid_new(int height, int width);
void grid_cpy(grid_t *dest, const grid_t *src);
void grid_block_add(grid_t *g, block_t *b);
void grid_block_remove(grid_t *g, block_t *b);
int grid_block_center_elevate(grid_t *g, block_t *b);
bool grid_block_intersects(grid_t *g, block_t *b);
int grid_block_drop(grid_t *g, block_t *b);
void grid_block_move(grid_t *g, block_t *b, direction_t d, int amount);
void grid_block_rotate(grid_t *g, block_t *b, int amount);
int grid_clear_lines(grid_t *g);

#define GRID_WIDTH 14
#define GRID_HEIGHT 20

typedef struct {
    uint8_t max_len;
    int iter;
    bool *defined;
    shape_t **stream;
} shape_stream_t;

shape_stream_t *shape_stream_new(void);
shape_t *shape_stream_peek(shape_stream_t *stream, int idx);
shape_t *shape_stream_pop(shape_stream_t *stream);

typedef enum {
    INPUT_INVALID,
    INPUT_TOGGLE_MODE,
    INPUT_PAUSE,
    INPUT_QUIT,
    INPUT_ROTATE,
    INPUT_MOVE_LEFT,
    INPUT_MOVE_RIGHT,
    INPUT_DROP,
} input_t;

void tui_grid_print(const grid_t *g);
void tui_build_display_buffer(const grid_t *g, block_t *falling_block);
void tui_render_display_buffer(const grid_t *g);
void tui_force_display_buffer_refresh(void);
void tui_block_print(block_t *b, int color, int grid_height);
void tui_block_print_shadow(block_t *b, int color, grid_t *g);
void tui_block_print_preview(block_t *b, int color);
void tui_add_block_color(block_t *b, int color);
void tui_prepare_color_preservation(const grid_t *g);
void tui_apply_color_preservation(const grid_t *g);
void tui_clear_lines_colors(const grid_t *g);
void tui_force_redraw(const grid_t *g);
void tui_force_grid_redraw(void);
void tui_periodic_cleanup(const grid_t *g);
void tui_refresh_borders(const grid_t *g);
void tui_update_stats(int level, int points, int lines_cleared);
void tui_update_mode_display(bool is_ai_mode);
void tui_flash_completed_lines(const grid_t *g,
                               int *completed_rows,
                               int num_completed);
void tui_validate_line_clearing(const grid_t *g);
void tui_setup(const grid_t *g);
void tui_prompt(const grid_t *g, const char *msg);
void tui_refresh(void);
void tui_quit(void);
input_t tui_scankey(void);
void tui_show_falling_pieces(const grid_t *g);

/* Consolidated color assignment function */
int tui_get_shape_color(shape_t *shape);

/* Shape access function for falling pieces effect */
shape_t *get_shape_by_index(int index);

float *default_weights();
move_t *best_move(grid_t *g, block_t *b, shape_stream_t *ss, float *w);
void auto_play(float *w);

/* Memory management cleanup functions */
void move_cleanup_atexit(void);
void free_shape(void);
