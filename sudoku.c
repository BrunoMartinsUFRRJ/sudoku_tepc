#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <omp.h>
#include <pthread.h>
#include <mpi.h>

#define INT_TYPE unsigned long long 
#define INT_TYPE_SIZE (sizeof(INT_TYPE) * 8)
#define CELL_VAL_SIZE 1
//MAX_BDIM = floor(sqrt(CELL_VAL_SIZE * INT_TYPE_SIZE)). Current value set for 64-bit INT_TYPE, adjust if needed
#define MAX_BDIM 8

enum SOLVE_STRATEGY {SUDOKU_SOLVE, SUDOKU_COUNT_SOLS};
#ifndef SUDOKU_SOLVE_STRATEGY
#define SUDOKU_SOLVE_STRATEGY SUDOKU_SOLVE
#endif

#define BUILD_ERROR_IF(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
void BUILD_TIME_CHECKS() {
    BUILD_ERROR_IF(INT_TYPE_SIZE * CELL_VAL_SIZE < MAX_BDIM * MAX_BDIM);
}

typedef struct cellval {
    INT_TYPE v[CELL_VAL_SIZE];
} cell_v;

typedef struct cell_coord {
    int r,c;
} cell_coord;

typedef struct sudoku {
    int bdim;
    int dim;
    int peers_size;
    int* grid;
    
    cell_coord ****unit_list; //[r][c][0 - row, 1 - column, 2 - box],
    cell_coord ***peers;
    cell_v **values;
    
    unsigned long long sol_count;
    int status;
} sudoku;

typedef struct pSquares
{
    int qtd;
    int k;
} pSquares;

bool jaDividiuProcessos = false, terminouLocalmente = false;
int world_size, world_rank, alguemTerminou = 0, flag[] = {0, 0, 0, 0, 0, 0};
pthread_t id;
MPI_Request polling;

void* initPolling (void* args) {

    MPI_Wait(&polling, MPI_STATUS_IGNORE);
    alguemTerminou = 1;
    return NULL;
}

static int assign (sudoku *s, int i, int j, int d);
static int search (sudoku *s, int argMinI, int argMinJ, int argK);

static inline int cell_v_get(cell_v *v, int p) {
    return !!((*v).v[(p - 1) / INT_TYPE_SIZE] & (((INT_TYPE)1) << ((p - 1) % INT_TYPE_SIZE))); //!! otherwise p > 32 breaks the return
}

static inline void cell_v_unset(cell_v *v, int p) {
    (*v).v[(p - 1) / INT_TYPE_SIZE] &= ~(((INT_TYPE)1) << ((p - 1) % INT_TYPE_SIZE));
}

static inline void cell_v_set(cell_v *v, int p) {
    (*v).v[(p - 1) / INT_TYPE_SIZE] |= ((INT_TYPE)1) << ((p -1) % INT_TYPE_SIZE);
}

static inline int cell_v_count(cell_v *v) {
    int acc = 0;
    for (int i = 0; i < CELL_VAL_SIZE; i++) 
        acc += __builtin_popcountll((*v).v[i]);
    return acc;
}

static inline int digit_get (cell_v *v) {
    int count = cell_v_count(v);
    if (count != 1) return -1;
    for (int i = 0; i < CELL_VAL_SIZE; i++) 
        if ((*v).v[i]) return 1 + INT_TYPE_SIZE * i + __builtin_ctzll((*v).v[i]);
    return -1;
}

static void destroy_sudoku(sudoku *s) {
    #pragma omp parallel for
    for (int i = 0; i < s->dim; i++) {
        for (int j = 0; j < s->dim; j++) {
            for (int k = 0; k < 3; k++)
                free(s->unit_list[i][j][k]);
            free(s->unit_list[i][j]);
        }
        free(s->unit_list[i]);
    }
    free(s->unit_list);
    
    #pragma omp parallel for
    for (int i = 0; i < s->dim; i++) {
        for (int j = 0; j < s->dim; j++)
            free(s->peers[i][j]);
        free(s->peers[i]);
    }
    free(s->peers);
    
    #pragma omp parallel for
    for (int i = 0; i < s->dim; i++) 
        free(s->values[i]);
    free(s->values);
    
    free(s);
}

static void init(sudoku *s) {
    int i, j, k, l, pos;
    
    //unit list 
    for (i = 0; i < s->dim; i++) {
        int ibase = i / s->bdim * s->bdim;
        for (j = 0; j < s->dim; j++) {
            for (pos = 0; pos < s->dim; pos++) {
                s->unit_list[i][j][0][pos].r = i; //row 
                s->unit_list[i][j][0][pos].c = pos;
                s->unit_list[i][j][1][pos].r = pos; //column
                s->unit_list[i][j][1][pos].c = j;
            }
            int jbase = j / s->bdim * s->bdim;
            for (pos = 0, k = 0; k < s->bdim; k++) //box
                for (l = 0; l < s->bdim; l++, pos++) {
                    s->unit_list[i][j][2][pos].r = ibase + k;
                    s->unit_list[i][j][2][pos].c = jbase + l;
                }
        }
    }
    
    //peers
    for (i = 0; i < s->dim; i++)
        for (j = 0; j < s->dim; j++) {
            pos = 0;
            for (k = 0; k < s->dim; k++) { //row
                if (s->unit_list[i][j][0][k].c != j)
                    s->peers[i][j][pos++] = s->unit_list[i][j][0][k]; 
            }
            for (k = 0; k < s->dim; k++) { 
                cell_coord sq = s->unit_list[i][j][1][k]; //column
                if (sq.r != i)
                    s->peers[i][j][pos++] = sq; 
                sq = s->unit_list[i][j][2][k]; //box
                if (sq.r != i && sq.c != j)
                    s->peers[i][j][pos++] = sq; 
            }
        }
    assert(pos == s->peers_size);
}

static int parse_grid(sudoku *s) {
    int i, j, k;
    int ld_vals[s->dim][s->dim];
    for (k = 0, i = 0; i < s->dim; i++)
        for (j = 0; j < s->dim; j++, k++) {
            ld_vals[i][j] = s->grid[k];
        }
    
    for (i = 0; i < s->dim; i++)
        for (j = 0; j < s->dim; j++)
            for (k = 1; k <= s->dim; k++)
                cell_v_set(&s->values[i][j], k);
    
    for (i = 0; i < s->dim; i++)
        for (j = 0; j < s->dim; j++)
            if (ld_vals[i][j] > 0 && !assign(s, i, j, ld_vals[i][j]))
                return 0;

    return 1;
}

static sudoku *create_sudoku(int bdim, int *grid) {
    assert(bdim <= MAX_BDIM);
    
    sudoku *r = malloc(sizeof(sudoku));
    r->bdim = bdim;
    int dim = bdim * bdim;
    r->dim = dim;
    r->peers_size = 3 * dim - 2 * bdim - 1;
    r->grid = grid;
    r->sol_count = 0;
    
    //[r][c][0 - row, 1 - column, 2 - box]//[r][c][0 - row, 1 - column, 2 - box][ix]
    r->unit_list = malloc(sizeof(cell_coord***) * dim);
    assert(r->unit_list);

    #pragma omp parallel for
    for (int i = 0; i < dim; i++) {
        r->unit_list[i] = malloc(sizeof(cell_coord**) * dim);
        assert (r->unit_list[i]);
        for (int j = 0; j < dim; j++) {
            r->unit_list[i][j] = malloc(sizeof(cell_coord*) * 3);
            assert(r->unit_list[i][j]);
            for (int k = 0; k < 3; k++) {
                r->unit_list[i][j][k] = calloc(dim, sizeof(cell_coord));
                assert(r->unit_list[i][j][k]);
            }
        }
    }
    
    r->peers = malloc(sizeof(cell_coord**) * dim);
    assert(r->peers);

    #pragma omp parallel for
    for (int i = 0; i < dim; i++) {
        r->peers[i] = malloc(sizeof(cell_coord*) * dim);
        assert(r->peers[i]);
        for (int j = 0; j < dim; j++) {
            r->peers[i][j] = calloc(r->peers_size, sizeof(cell_coord));
            assert(r->peers[i][j]);
        }
    }
    
    r->values = malloc (sizeof(cell_v*) * dim);
    assert(r->values);

    #pragma omp parallel for
    for (int i = 0; i < dim; i++) {
        r->values[i] = calloc(dim, sizeof(cell_v));
        assert(r->values[i]);
    }
    
    init(r);
    if (!parse_grid(r)) {
        printf("Error parsing grid\n");
        destroy_sudoku(r);
        return 0;
    }
    
    return r;
}

static int eliminate (sudoku *s, int i, int j, int d) {
    int k, ii, cont, pos;
    if (!cell_v_get(&s->values[i][j], d)) 
        return 1;

    cell_v_unset(&s->values[i][j], d);

    int count = cell_v_count(&s->values[i][j]);
    if (count == 0) {
        return 0;
    } else if (count == 1) {
        for (k = 0; k < s->peers_size; k++) {
            if (!eliminate(s, s->peers[i][j][k].r, s->peers[i][j][k].c, digit_get(&s->values[i][j])))
                return 0;
        }
    }

    for (k = 0; k < 3; k++) {//row, column, box 
        cont = 0;
        pos = 0;
        cell_coord* u = s->unit_list[i][j][k];
        for (ii = 0; ii < s->dim; ii++) {
            if (cell_v_get(&s->values[u[ii].r][u[ii].c], d)) {
                cont++;
                pos = ii;
            }
        }
        if (cont == 0)
            return 0;
        else if (cont == 1) {
            if (!assign(s, u[pos].r, u[pos].c, d))
                return 0;
        }
    }
    return 1;
}

static int assign (sudoku *s, int i, int j, int d) {
    for (int d2 = 1; d2 <= s->dim; d2++)
        if (d2 != d) 
            if (!eliminate(s, i, j, d2))
               return 0;
    return 1;
}

static void display(sudoku *s) {
    printf("%d\n", s->bdim);
    for (int i = 0; i < s->dim; i++)
        for (int j = 0; j < s->dim; j++)
            printf("%d ",  digit_get(&s->values[i][j]));
}

int apresentarResultados (sudoku * copia) {
    display(copia);
    MPI_Cancel(&polling);
    terminouLocalmente = true;
    alguemTerminou = 1;
    MPI_Send(&alguemTerminou, 1, MPI_INT, world_rank == 0 ? 1 : 0, 123, MPI_COMM_WORLD);
    return 1;
}

void encontrarSquareMenosPossibilidades (sudoku *s, int *min, int *minI, int *minJ) {
    for (int i = 0; i < s->dim; i++) 
        for (int j = 0; j < s->dim; j++) {
            int used = cell_v_count(&s->values[i][j]);
            if (used > 1 && used < *min) {
                *min = used;
                *minI = i;
                *minJ = j;
            }
        }
}

void encontrarSquareNumProcessadores (sudoku *s, int *min, int *minI, int *minJ) {
    int wanted = omp_get_num_procs(); 
    for (int i = 0; i < s->dim; i++) 
        for (int j = 0; j < s->dim; j++) {
            int used = cell_v_count(&s->values[i][j]);
            if (wanted == used) {
                *min = used;
                *minI = i;
                *minJ = j;
                break;
            }
        }
}

sudoku* copiarSudoku (sudoku *s) {
    sudoku *copia = malloc(sizeof(sudoku));
    copia->status = 1;
    memcpy(copia, s, sizeof(sudoku));
    copia->values = malloc (sizeof (cell_v *) * s->dim);
    for (int i = 0; i < s->dim; i++) {
        copia->values[i] = malloc (sizeof (cell_v) * s->dim);
        memcpy(copia->values[i], s->values[i], sizeof (cell_v) * s->dim);
    }
    return copia;
}

int fazerTarefas (sudoku *s, int nsudokus, int minI, int minJ, pSquares possibilidades []) {
    
    sudoku* vetoresSudoku[nsudokus];

    #pragma omp parallel
    {
        #pragma omp for
        for (int a = 0; a < nsudokus; a++) {
            vetoresSudoku[a] = copiarSudoku(s);
            vetoresSudoku[a]->status = assign(vetoresSudoku[a], minI, minJ, possibilidades[a].k);
        }

        #pragma omp single nowait
        for (int a = 0; a < nsudokus; a++) {
            if (vetoresSudoku[a]->status) {
                int min = INT_MAX, minI, minJ;
                encontrarSquareNumProcessadores(vetoresSudoku[a], &min, &minI, &minJ);

                for (int k = 1; k <= vetoresSudoku[a]->dim; k++) {
                    if (cell_v_get(&vetoresSudoku[a]->values[minI][minJ], k)) {
                        #pragma omp task
                        {
                            vetoresSudoku[a] = copiarSudoku(vetoresSudoku[a]);
                            search(vetoresSudoku[a], minI, minJ, k);
                        }
                    }
                }
            }
        }

        #pragma omp taskwait
    }

    return terminouLocalmente ? 1 : 0;
}

int cmpSquare (const void *a, const void *b) {
    return ((pSquares *) b)->qtd - ((pSquares *) a)->qtd;
}

static int search (sudoku *s, int argMinI, int argMinJ, int argK) {
    int i, j, k;
    if (alguemTerminou || terminouLocalmente) return 0;

    int status = argK > 0 ? assign(s, argMinI, argMinJ, argK) : 1;
    if (!status) return 0;

    int solved = 1;
    for (i = 0; solved && i < s->dim; i++) 
        for (j = 0; j < s->dim; j++) 
            if (cell_v_count(&s->values[i][j]) != 1) {
                solved = 0;
                break;
            }
    if (solved) {
        s->sol_count++;
        s->status = 1;
        apresentarResultados(s);
        return 1;
    }

    //ok, there is still some work to be done
    int min = INT_MAX;
    int minI = -1;
    int minJ = -1;
    int ret = 0;
    
    cell_v **values_bkp = malloc (sizeof (cell_v *) * s->dim);
    for (i = 0; i < s->dim; i++)
        values_bkp[i] = malloc (sizeof (cell_v) * s->dim);

    encontrarSquareMenosPossibilidades(s, &min, &minI, &minJ);

    if (!jaDividiuProcessos) {
        jaDividiuProcessos = true;

        const int nitems = 2;
        int blocklenghts [] = {1, 1};
        MPI_Datatype types [] = {MPI_INT, MPI_INT};
        MPI_Datatype mpi_psquare_type;
        MPI_Aint offsets[2];

        offsets[0] = offsetof(pSquares, qtd);
        offsets[1] = offsetof(pSquares, k);

        MPI_Type_create_struct(nitems, blocklenghts, offsets, types, &mpi_psquare_type);
        MPI_Type_commit(&mpi_psquare_type);
        
        if (world_rank == 1) {
            pSquares possibilidades[min];
            
            int a = 0;
            for (k = 1; k <= s->dim; k++) {
                if (cell_v_get(&s->values[minI][minJ], k))  {
                    int q = 0; // contador do aparecimento de k nos quadrados do tabuleiro
                    for (i = 0; i < s->dim; i++) {
                        for (j = 0; j < s->dim; j++) {
                            if(cell_v_get(&s->values[i][j], k)) q++;
                        }
                    }
                    possibilidades[a].qtd = q, possibilidades[a].k = k;
                    a++;
                }
            }

            qsort(possibilidades, min, sizeof(pSquares), cmpSquare);

            if (min % 2 == 0) MPI_Send(possibilidades + (min / 2), min / 2, mpi_psquare_type, 0, 0, MPI_COMM_WORLD);
            else MPI_Send(possibilidades + (min / 2) + 1, min / 2, mpi_psquare_type, 0, 0, MPI_COMM_WORLD);

            MPI_Type_free(&mpi_psquare_type);
            pthread_create(&id, NULL, initPolling, NULL);
            if (fazerTarefas (s, min / 2, minI, minJ, possibilidades)) return 1;
            pthread_cancel(id);
            MPI_Wait(&polling, MPI_STATUS_IGNORE);
            return 0;
        } else { 

            int number_amount;
            MPI_Status status;
            MPI_Probe(1, 0, MPI_COMM_WORLD, &status);
            MPI_Get_count(&status, mpi_psquare_type, &number_amount);

            pSquares possibilidades[number_amount];
            MPI_Recv(possibilidades, number_amount, mpi_psquare_type, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            MPI_Type_free(&mpi_psquare_type);
            pthread_create(&id, NULL, initPolling, NULL);
            if (fazerTarefas(s, number_amount, minI, minJ, possibilidades)) return 1;
            pthread_cancel(id);
            MPI_Wait(&polling, MPI_STATUS_IGNORE);
            return 0;
        }

        return 0;

    } else {            
        for (k = 1; k <= s->dim; k++) {
            if (cell_v_get(&s->values[minI][minJ], k))  {
                for (i = 0; i < s->dim; i++)
                    for (j = 0; j < s->dim; j++)
                        values_bkp[i][j] = s->values[i][j];
                
                if (search (s, minI, minJ, k)) {
                    ret = 1;
                    goto FR_RT;
                } else {
                    for (i = 0; i < s->dim; i++) 
                        for (j = 0; j < s->dim; j++)
                            s->values[i][j] = values_bkp[i][j];
                }
            }
        }
    }

    FR_RT:
    for (i = 0; i < s->dim; i++)
        free(values_bkp[i]);
    free (values_bkp);
    
    return ret;
}

int solve(sudoku *s) {
    return search(s, -1, -1, 0);
}

int main (int argc, char **argv) {

    MPI_Init(NULL, NULL);

    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    MPI_Irecv(&alguemTerminou, 1, MPI_INT, world_rank == 1 ? 0 : 1, 123, MPI_COMM_WORLD, &polling);

    sudoku *s;

    if (world_rank == 0) {
        int size;
        assert(scanf("%d", &size) == 1);
        assert (size <= MAX_BDIM);
        int buf_size = size * size * size * size;
        int buf[buf_size];

        for (int i = 0; i < buf_size; i++) {
            if (scanf("%d", &buf[i]) != 1) {
                printf("error reading file (%d)\n", i);
                exit(1);
            }
        }

        MPI_Send(&size, 1, MPI_INT, 1, 0, MPI_COMM_WORLD);
        MPI_Send(&buf, buf_size, MPI_INT, 1, 0, MPI_COMM_WORLD);

        s = create_sudoku(size, buf);
    } else {
        int size;

        MPI_Recv(&size, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        int buf_size = size * size * size * size;
        int buf[buf_size];
        MPI_Recv(&buf, buf_size, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);        

        s = create_sudoku(size, buf);
    } 
    if (s) {
        int result = solve(s);
        if (!result && alguemTerminou == 0 && terminouLocalmente == false) printf("Could not solve puzzle.\n");
        destroy_sudoku(s);
    } else {
        printf("Could not load puzzle.\n");
    }

    MPI_Finalize();

    return 0;
}