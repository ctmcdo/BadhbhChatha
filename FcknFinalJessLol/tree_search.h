#include <assert.h>
#include <gmp.h>
#include <immintrin.h>
#include <inttypes.h>
#include <nmmintrin.h>
#include <stdio.h>

#include "chess.h"
#include "prom_slack.h"
#include "tree_common.h"

#define EDGE_ROWS_MASK                                                                             \
    -1 - ((1UL << (7 * BOARD_SIDE_LENGTH)) - 1) + ((1UL << BOARD_SIDE_LENGTH) - 1)

// 0 = (a, b, c, d)
// 1   = (a, a, b, c)
// 2     = (a, a, a, b)
// 3       = (a, a, a, a)
// 4     = (a, a, b, b)
// 5   = (a, b, b, c)
// 6     = (a, b, b, b)
// 7   = (a, b, c, c)
int FUNDAMENTAL_SET_DIVISORS[]
    = { 1, 2, THREE_FACTORIAL, FOUR_FACTORIAL, (2 * 2), 2, THREE_FACTORIAL, 2 };

int BASE_PIECES[NUM_FIXED_ROOK_SCENARIOS][NUM_PIECE_TYPES_LESS_KING];

typedef struct
{
    int indices[NUM_SIDES];
} permutation_indices;

int point_root_to_matching_child(position_node** root, mpz_t index)
{
    int i = 0;
    for (; mpz_cmp(index, (*root)->children[i]->num_positions) >= 0; i++)
    {
        mpz_sub(index,
                index,                                // if rng >= positions in subtree
                (*root)->children[i]->num_positions); // then crashing is acceptable
    }                                                 // because there's a bug
    assert(i < (*root)->num_children);

    *root = (*root)->children[i];

    return i;
}

uint64_t pass_enpassant(position_node** root, mpz_t index, position* p, uint64_t* occupied_squares)
{
    mpz_t rem;
    mpz_init(rem);
    int      remui;
    uint64_t unoccupiable_adjacent = 0;

    int enpassant_case = point_root_to_matching_child(root, index);
    if (enpassant_case == ENPASSANT_EDGE_AND_RIGHT)
    {
        mpz_fdiv_qr_ui(index, rem, index, ENPASSANT_EDGE_AND_RIGHT_VARIATIONS);
        remui = mpz_get_ui(rem);

        if (remui != ENPASSANT_EDGE_AND_RIGHT_VARIATIONS - 1)
        {
            p->enpassant      = 1UL << ((ENPASSANT_ROW_0INDEX + 1) * BOARD_SIDE_LENGTH - 1);
            p->sides[1].pawns = p->enpassant >> 1;
        }
        else
        {
            p->enpassant      = 1UL << (ENPASSANT_ROW_0INDEX * BOARD_SIDE_LENGTH + remui);
            p->sides[1].pawns = p->enpassant >> 1;
        }
    }
    else if (enpassant_case == ENPASSANT_LEFT_LESS_EDGE)
    {
        mpz_fdiv_qr_ui(index, rem, index, ENPASSANT_LEFT_LESS_EDGE_VARIATIONS);
        remui = mpz_get_ui(rem);

        p->enpassant      = 1UL << (ENPASSANT_ROW_0INDEX * BOARD_SIDE_LENGTH + 1 + remui);
        p->sides[1].pawns = p->enpassant << 1;
    }
    else if (enpassant_case != NO_ENPASSANT)
    {
        assert(false);
    }

    p->sides[0].pawns = p->enpassant;
    *occupied_squares = p->sides[0].pawns + p->sides[1].pawns + unoccupiable_adjacent;
    if (enpassant_case != NO_ENPASSANT)
    {
        *occupied_squares
            += ((*p).enpassant >> BOARD_SIDE_LENGTH) + ((*p).enpassant >> (2 * BOARD_SIDE_LENGTH));
    }

    mpz_clear(rem);

    return unoccupiable_adjacent;
}

uint64_t place_chessmen_relative_to_free_squares(int      num_chessmen,
                                                 int      num_free_squares,
                                                 uint64_t index)
{
    if (num_chessmen == 0)
    {
        return 0;
    }

    for (int i = 0; i < num_free_squares - (num_chessmen - 1); i++)
    {
        if (index < binomials[num_free_squares - 1 - i][num_chessmen - 1])
        {
            uint64_t further_chessmen = place_chessmen_relative_to_free_squares(
                                            num_chessmen - 1, num_free_squares - 1 - i, index)
                                        << (i + 1);
            return (1UL << i) + further_chessmen;
        }

        index -= binomials[num_free_squares - 1 - i][num_chessmen - 1];
    }
    printf("Couldn't place chessmen\n");
    exit(1);
}

int pass_generic(position_node** root, mpz_t index, uint64_t* occupied_squares, uint64_t* bitboard)
{
    int num_chessmen = point_root_to_matching_child(root, index);
    if (num_chessmen == 0)
    {
        return 0;
    }

    mpz_t rem;
    mpz_init(rem);

    int num_free_squares = NUM_SQUARES - _mm_popcnt_u64(*occupied_squares);
    mpz_fdiv_qr_ui(index, rem, index, binomials[num_free_squares][num_chessmen]);
    uint64_t chessmen = _pdep_u64(
        place_chessmen_relative_to_free_squares(num_chessmen, num_free_squares, mpz_get_ui(rem)),
        ~(*occupied_squares));
    *bitboard += chessmen;
    *occupied_squares += chessmen;

    mpz_clear(rem);

    return num_chessmen;
}

int pass_fixed_rooks_and_kings(position_node** root,
                               mpz_t           index,
                               position*       p,
                               uint64_t*       occupied_squares,
                               bool            side)
{
    mpz_t rem;
    mpz_init(rem);
    uint64_t remui;

    uint64_t fixed_rooks     = 0;
    int      num_fixed_rooks = point_root_to_matching_child(root, index);
    switch (num_fixed_rooks)
    {
    case NO_CASTLING_RIGHTS: break;

    case CASTLING_RIGHTS_ONE_SIDE:
        mpz_fdiv_qr_ui(index, rem, index, ONE_FIXED_ROOK_VARIATIONS);
        remui       = mpz_get_ui(rem);
        fixed_rooks = 1UL << ((BOARD_SIDE_LENGTH - 1) * remui);
        break;

    case CASTLING_RIGHTS_BOTH_SIDES: fixed_rooks = 1 + (1UL << (BOARD_SIDE_LENGTH - 1)); break;

    default: printf("Error switching on num_fixed_rooks\n"); exit(1);
    }

    uint64_t king = 0;
    if (num_fixed_rooks > 0)
    {
        king = rcb(0, KING_HOME_COLUMN_0INDEX);
        if (side)
        {
            fixed_rooks = rotate_bitboard_across_central_rows(fixed_rooks);
            king        = rotate_bitboard_across_central_rows(king);
        }
    }

    p->sides[side]._fr = fixed_rooks;
    p->fixed_rooks += fixed_rooks;
    p->sides[side].pieces[KING] = king;
    *occupied_squares += fixed_rooks + king;

    mpz_clear(rem);

    return num_fixed_rooks;
}

permutation_indices get_permutation_index(int      pawn_slack[NUM_SIDES],
                                          int      chessmen_slack,
                                          int*     cost_boundaries[NUM_SIDES],
                                          int      fundamentalSet_indices[NUM_SIDES],
                                          uint32_t index)
{
    int max_addn_cost0 = min3(pawn_slack[0], chessmen_slack, MAX_UNIQUE_COSTS - 1);
    int i              = 0;
    for (; i < (max_addn_cost0 + 1); i++)
    { // costs are 1 apart
        int p0 = cost_boundaries[0][i] / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[0]];
        assert(p0 != -1);
        if (i > 0)
        {
            p0 -= cost_boundaries[0][i - 1] / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[0]];
        }

        int max_addn_cost1 = min3(pawn_slack[1], chessmen_slack - i, MAX_UNIQUE_COSTS - 1);
        for (int j = 0; j < (max_addn_cost1 + 1); j++)
        {
            int p1 = cost_boundaries[1][j] / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[1]];
            assert(p1 != -1);
            if (j > 0)
            {
                p1 -= cost_boundaries[1][j - 1]
                      / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[1]];
            }
            int d = index - (p0 * p1);
            if (d < 0)
            {
                permutation_indices pi;
                int                 offset0 = (int)(index / p1);
                pi.indices[0]               = offset0;
                if (i > 0)
                {
                    pi.indices[0] += cost_boundaries[0][i - 1]
                                     / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[0]];
                }
                pi.indices[1] = (index - (offset0 * p1));
                if (j > 0)
                {
                    pi.indices[1] += cost_boundaries[1][j - 1]
                                     / FUNDAMENTAL_SET_DIVISORS[fundamentalSet_indices[1]];
                }
                return pi;
            }

            index = d;
        }
    }
    printf("Couldn't get permutation index\n");
    exit(1);
}

int get_fundamentalSet_index(int nfp[NUM_PIECE_TYPES_LESS_KING])
{
    if (nfp[1] == nfp[0])
    {
        if (nfp[2] == nfp[1])
        {
            if (nfp[3] == nfp[2])
            {
                // 3       = (a, a, a, a)
                return 3;
            }
            else
            {
                // 2     = (a, a, a, b)
                return 2;
            }
        }
        else
        {

            if (nfp[3] == nfp[2])
            {
                // 4     = (a, a, b, b)
                return 4;
            }
            else
            {
                // 1   = (a, a, b, c)
                return 1;
            }
        }
    }
    else
    {
        if (nfp[2] == nfp[1])
        {
            if (nfp[3] == nfp[2])
            {
                // 6     = (a, b, b, b)
                return 6;
            }
            else
            {
                // 5   = (a, b, b, c)
                return 5;
            }
        }
        else
        {
            if (nfp[3] == nfp[2])
            {
                // 7   = (a, b, c, c)
                return 7;
            }
            else
            {

                // 0 = (a, b, c, d)
                return 0;
            }
        }
    }
    exit(1);
}

// Return a chessboard indexed by the GMP mpz_t 'index'
position retrieve_position_from_rng(position_node* root, mpz_t index)
{
    position p = { 0 };

    mpz_t rem;
    mpz_init(rem);
    mpz_fdiv_qr_ui(index, rem, index, 2);
    if (mpz_get_ui(rem))
    {
        p.side0isBlack = true;
    }

    uint64_t occupied_squares = 0;
    // tmp_unoccupiable_adjacent is used to block the currently empty adjacent
    // in the case of ENPASSANT_ONE_ADJACENT
    uint64_t tmp_unoccupiable_enpassant_adjacent
        = pass_enpassant(&root, index, &p, &occupied_squares);

    // Pawns can't be placed on edge rows
    occupied_squares += EDGE_ROWS_MASK;
    pass_generic(&root, index, &occupied_squares, &p.sides[1].pawns);

    occupied_squares -= tmp_unoccupiable_enpassant_adjacent;
    pass_generic(&root, index, &occupied_squares, &p.sides[0].pawns);
    bool equal_num_pawns = _mm_popcnt_u64(p.sides[0].pawns) == _mm_popcnt_u64(p.sides[1].pawns);
    if (!p.enpassant && !equal_num_pawns)
    {
        mpz_fdiv_qr_ui(index, rem, index, 2);
        if (mpz_get_ui(rem))
        {
            p.side0toMove = true;
        }
    }
    occupied_squares -= EDGE_ROWS_MASK;
    int nfr[NUM_SIDES];
    nfr[0] = pass_fixed_rooks_and_kings(&root, index, &p, &occupied_squares, 0);
    nfr[1] = pass_fixed_rooks_and_kings(&root, index, &p, &occupied_squares, 1);
    if (!p.enpassant && equal_num_pawns && (nfr[0] != nfr[1]))
    {
        mpz_fdiv_qr_ui(index, rem, index, 2);
        if (mpz_get_ui(rem))
        {
            p.side0toMove = true;
        }
    }

    int num_non_fixed_pieces_less_king[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING]      = { 0 };
    int num_non_fixed_base_pieces_less_king[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING] = { 0 };
    int promotions[NUM_SIDES]                                                     = { 0 };
    for (int i = 0; i < NUM_SIDES; i++)
    {
        for (int j = 0; j < NUM_PIECE_TYPES_LESS_KING; j++)
        {
            num_non_fixed_pieces_less_king[i][j]
                = pass_generic(&root, index, &occupied_squares, &p.sides[i].pieces[j]);
            num_non_fixed_base_pieces_less_king[i][j] = num_non_fixed_pieces_less_king[i][j];
            int d = num_non_fixed_pieces_less_king[i][j] - BASE_PIECES[nfr[i]][j];
            if (d > 0)
            {
                num_non_fixed_base_pieces_less_king[i][j] = BASE_PIECES[nfr[i]][j];
                promotions[i] += d;
            }
            if (num_non_fixed_pieces_less_king[i][j] == 0)
            {
                break;
            }
        }
    }

    for (int i = 0; i < NUM_SIDES; i++)
    {
        if (nfr[i] == NO_CASTLING_RIGHTS)
        {
            int num_free_squares = NUM_SQUARES - _mm_popcnt_u64(occupied_squares);
            mpz_fdiv_qr_ui(index, rem, index, num_free_squares);
            p.sides[i].pieces[KING] = _pdep_u64(
                place_chessmen_relative_to_free_squares(1, num_free_squares, mpz_get_ui(rem)),
                ~(occupied_squares));
            occupied_squares += p.sides[i].pieces[KING];
        }
    }
    mpz_clear(rem);

    int num_pawns[NUM_SIDES];
    int total_base_capturable_pieces[NUM_SIDES];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        num_pawns[i] = _mm_popcnt_u64(p.sides[i].pawns);

        total_base_capturable_pieces[i] = nfr[i];
        for (int j = 0; j < NUM_PIECE_TYPES_LESS_KING; j++)
        {
            total_base_capturable_pieces[i] += num_non_fixed_base_pieces_less_king[i][j];
        }
    }

    int covered_sets[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        for (int j = 0; j < NUM_PIECE_TYPES_LESS_KING; j++)
        {
            covered_sets[i][j] = num_non_fixed_pieces_less_king[i][j];
            if (covered_sets[i][j] > 2)
            {
                covered_sets[i][j] = 2;
            }
        }
    }

    int  coveredSet_indices[NUM_SIDES];
    int  fundamentalSet_indices[NUM_SIDES];
    int* cost_boundary_indices[NUM_SIDES];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        coveredSet_indices[i] = coveredSet_index[covered_sets[i][0]][covered_sets[i][1]]
                                                [covered_sets[i][2]][covered_sets[i][3]];
        fundamentalSet_indices[i] = get_fundamentalSet_index(num_non_fixed_pieces_less_king[i]);
        cost_boundary_indices[i]
            = fr_coveredSetIndex_permAddnCost_numPerms[nfr[i]][coveredSet_indices[i]];
    }
    slack prom_slack = promotion_slack(num_pawns, total_base_capturable_pieces, promotions);
    permutation_indices pi
        = get_permutation_index(prom_slack.pawn_slack,
                                min(prom_slack.chessmen_slack[0], prom_slack.chessmen_slack[1]),
                                cost_boundary_indices,
                                fundamentalSet_indices,
                                mpz_get_ui(index));

    int* permutations[NUM_SIDES];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        permutations[i]
            = fr_coveredSetIndex_fundamentalSetIndex_combIndex_comb[nfr[i]][coveredSet_indices[i]]
                                                                   [fundamentalSet_indices[i]]
                                                                   [pi.indices[i]];
    }
    assert(permutations[0][0] != -1);
    assert(permutations[1][0] != -1);

    uint64_t pieces_tmp[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        for (int j = 0; j < NUM_PIECE_TYPES_LESS_KING; j++)
        {
            pieces_tmp[i][j] = p.sides[i].pieces[j];
        }
    }

    for (int i = 0; i < NUM_SIDES; i++)
    {
        for (int j = 0; j < NUM_PIECE_TYPES_LESS_KING; j++)
        {
            p.sides[i].pieces[j] = pieces_tmp[i][permutations[i][j]];
        }
    }

    for (int i = 0; i < NUM_SIDES; i++)
    {
        if (nfr[i] == CASTLING_RIGHTS_BOTH_SIDES)
        {
            uint64_t swp             = p.sides[i].pieces[ROOK];
            p.sides[i].pieces[ROOK]  = p.sides[i].pieces[QUEEN];
            p.sides[i].pieces[QUEEN] = swp;
        }
    }

    p.sides[0].pieces[ROOK] += p.sides[0]._fr;
    p.sides[1].pieces[ROOK] += p.sides[1]._fr;

    return p;
}
