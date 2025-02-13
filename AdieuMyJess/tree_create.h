#include <assert.h>
#include <gmp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "chess.h"
#include "prom_slack.h"
#include "tree_common.h"

// I document this file from the end upwards, so you'd be best starting
// from the bottom.

uint64_t binomials[NUM_SQUARES + 1][MAX_BISHOPS_PSIDE + 1];

// Can have 0, 1, or 2 rooks with castling rights
// |{0, 1, 2}| = 3
int BASE_PIECES[NUM_FIXED_ROOK_SCENARIOS][NUM_PIECE_TYPES_LESS_KING]
    = { { 2, 2, 2, 1 }, { 2, 2, 1, 1 }, { 2, 2, 1, 0 } };
///////////// b, n, r, q    b, n, r, q    b, n, q, r
/////////////////////////////////////////////// <-->
//////////// Notice how we switch queens and rooks ^

int factorials[] = { 1, 1, 2, 6, FOUR_FACTORIAL };

typedef struct
{
    position_node* root;
    int            free_pawns[NUM_SIDES];
    int            fixed_rooks[NUM_SIDES];
    int            num_occupiable_squares;
    bool           enpassant;
    bool           side;
} create_tree_threading_struct;

int num_piece_type_permutations(int  pawn_slack[NUM_SIDES],
                                int  chessmen_slack,
                                int* cost_boundaries[NUM_SIDES])
{
    uint32_t variations     = 0;
    int      max_addn_cost0 = min3(pawn_slack[0], chessmen_slack, MAX_UNIQUE_COSTS - 1);
    for (int i = 0; i < (max_addn_cost0 + 1); i++)
    { // costs are 1 apart
        int p1 = cost_boundaries[0][i];
        if (i > 0)
        {
            p1 -= cost_boundaries[0][i - 1];
        }
        assert(p1 != -1);

        if (p1 > 0)
        {
            int max_addn_cost1 = min3(pawn_slack[1], chessmen_slack - i, MAX_UNIQUE_COSTS - 1);
            int p2             = cost_boundaries[1][max_addn_cost1];
            assert(p2 != -1);
            variations += p1 * p2;
        }
    }

    assert(variations > 0 && variations <= (24 * 24));
    return variations;
}

int piece_combinations_divisor(int non_fixed_capturable_pieces[NUM_SIDES][NUM_PIECE_TYPES])
{
    int divisor = 1;
    for (int i = 0; i < NUM_SIDES; i++)
    {
        int streak = 1;
        for (int j = 2; j < NUM_PIECE_TYPES; j++)
        {
            if (non_fixed_capturable_pieces[i][j] == non_fixed_capturable_pieces[i][j - 1])
            {
                ++streak;
            }
            else
            {
                divisor *= factorials[streak];
                streak = 1;
            }
        }
        divisor *= factorials[streak];
    }
    return divisor;
}

// Level 6 onwards represents generic pieces. We don't assign piece types
// until within the search tree.
void count_from_pieces_helper(position_node* root,
                              int            pawns[NUM_SIDES],
                              int            fixed_rooks[NUM_SIDES],
                              int       non_fixed_capturable_pieces[NUM_SIDES][NUM_PIECE_TYPES],
                              int       covered_sets[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING],
                              int       total_base_capturable_pieces[NUM_SIDES],
                              int       promotions[NUM_SIDES],
                              const int num_occupiable_squares,
                              const int rel_level)
{
    mpz_init(root->num_positions);

    // rel_level is used to determine side and piece_type
    const bool side       = rel_level > NUM_PIECE_TYPES_LESS_KING;
    const int  piece_type = (int)((rel_level - (side + 1)) % NUM_PIECE_TYPES_LESS_KING);

    slack prom_slack      = promotion_slack(pawns, total_base_capturable_pieces, promotions);
    int   piece_lim2      = prom_slack.chessmen_slack[side];
    int   max_base_pieces = BASE_PIECES[fixed_rooks[side]][piece_type];
    if (piece_lim2 > max_base_pieces)
    {
        piece_lim2 = max_base_pieces + (int)((piece_lim2 - max_base_pieces) / 2);
    }
    // We ensure there are sufficient captured pieces and pawns to cover the
    // opposition's promotions. This only applies to side1 ensuring side0's
    // promotions are valid because side0 is computed assuming side1 has nothing
    // but a king on the board
    int piece_lim3 = prom_slack.chessmen_slack[!side];
    // Pieces are assumed to be base pieces up to max_base_pieces. Otherwise by
    // considering a piece promoted where it could be base we would be decreasing
    // the number of pieces the side with the piece-in-question can have by 1 and
    // decreasing the opposition's potential promotions by 1
    if (piece_lim3 > max_base_pieces)
    {
        piece_lim3 = max_base_pieces + (int)((piece_lim3 - max_base_pieces) / 2);
    }
    int max_pieces = min3(max_base_pieces + prom_slack.pawn_slack[side], piece_lim2, piece_lim3);
    assert(0 <= max_pieces && max_pieces <= 10);

    // We can always have none of a piece, so there are 1 + max_pieces cases.
    //
    // We also limit the number of pieces to the previous number of pieces for
    // this side. As already mentioned in count_from_pieces, previous_pieces =
    // MAX_BISHOPS_PSIDE = 10 initially. We enforce p_n >= p_(n+1) to avoid
    // counting duplicate piece sets. We later consider the # of permutations of
    // piece types to these abstract piece sets
    root->num_children = min(1 + max_pieces, 1 + non_fixed_capturable_pieces[side][piece_type]);
    assert(0 <= root->num_children && root->num_children <= 11);

    root->children = (position_node**)malloc(root->num_children * sizeof(position_node*));
    for (int i = 0; i < root->num_children; i++)
    {
        root->children[i] = (position_node*)malloc(sizeof(position_node));
    }

    for (int i = 0; i < root->num_children; i++)
    {
        assert(0 <= num_occupiable_squares - i && num_occupiable_squares - i <= 64);

        non_fixed_capturable_pieces[side][piece_type + 1] = i;
        // covered sets denote potential to cover piece permutations.
        // For a fuller description see tree_piece_comb_gen.py
        covered_sets[side][piece_type] = non_fixed_capturable_pieces[side][piece_type + 1];
        if (covered_sets[side][piece_type] > MAX_OF_ANY_ONE_BASE_PIECE)
        {
            covered_sets[side][piece_type] = MAX_OF_ANY_ONE_BASE_PIECE;
        }

        int new_base_pieces = i;
        if (i > BASE_PIECES[fixed_rooks[side]][piece_type])
        {
            new_base_pieces = BASE_PIECES[fixed_rooks[side]][piece_type];
        }
        assert(0 <= new_base_pieces && new_base_pieces <= 2);
        assert(total_base_capturable_pieces[side] + new_base_pieces <= 8);

        int new_total_base_capturable_pieces[NUM_SIDES];
        new_total_base_capturable_pieces[!side] = total_base_capturable_pieces[!side];
        new_total_base_capturable_pieces[side]
            = total_base_capturable_pieces[side] + new_base_pieces;

        int new_promotions[NUM_SIDES];
        new_promotions[!side] = promotions[!side];
        new_promotions[side]  = promotions[side] + (i - new_base_pieces);

        // We're at a leaf
        if ((side == 1 && i == 0) || rel_level == (2 * (NUM_PIECE_TYPES)-1))
        {
            // cost boundaries is an index into permutations and specifies which
            // permutations cost what - where the cost is the additional slack
            // we need to rule in the piece permutation
            int* cost_boundary_indices[NUM_SIDES];
            for (int j = 0; j < NUM_SIDES; j++)
            {
                int csj = coveredSet_index[covered_sets[j][0]][covered_sets[j][1]]
                                          [covered_sets[j][2]][covered_sets[j][3]];
                cost_boundary_indices[j]
                    = fr_coveredSetIndex_permAddnCost_numPerms[fixed_rooks[j]][csj];
            }
            slack prom_slack
                = promotion_slack(pawns, new_total_base_capturable_pieces, new_promotions);
            // If a permutation utilises one less base piece, then it has one more
            // promotion. By looking at our constraint in prom_slack, we see that one
            // additional promotion and one less base piece decreases the chessmen
            // slack for both sides by 1. Therefore, we only need to pass in the
            // minimum of both side's chessmen slack. The same can't be said of pawn
            // slack: it's not affected by opposition promotions
            uint64_t variations = binomials[num_occupiable_squares][i];
            int      perms      = num_piece_type_permutations(
                prom_slack.pawn_slack,
                min(prom_slack.chessmen_slack[0], prom_slack.chessmen_slack[1]),
                cost_boundary_indices);
            
            // Account for same number of piece types such as 2 bishops and 2 knights
            int comb_divisor = piece_combinations_divisor(non_fixed_capturable_pieces);
            assert(perms % comb_divisor == 0);
            int piece_combinations = perms / comb_divisor;
            variations *= piece_combinations;

            int new_num_occupiable_squares = num_occupiable_squares - i;
            for (int j = 0; j < NUM_SIDES; j++)
            {
                if (fixed_rooks[j] == 0)
                {
                    variations *= new_num_occupiable_squares;
                    --new_num_occupiable_squares; // king
                }
            }

            mpz_init_set_ui(root->children[i]->num_positions, variations);
            mpz_add_ui(root->num_positions, root->num_positions, variations);
        }
        else
        { // We're not at a leaf
            int new_rel_level = rel_level + 1;
            if (i == 0 || new_rel_level == (NUM_PIECE_TYPES_LESS_KING + 1))
            {
                // Jump to side1
                new_rel_level = NUM_PIECE_TYPES_LESS_KING + 2;
            }
            count_from_pieces_helper(root->children[i],
                                     pawns,
                                     fixed_rooks,
                                     non_fixed_capturable_pieces,
                                     covered_sets,
                                     new_total_base_capturable_pieces,
                                     new_promotions,
                                     num_occupiable_squares - i,
                                     new_rel_level);
            mpz_mul_ui(root->children[i]->num_positions,
                       root->children[i]->num_positions,
                       binomials[num_occupiable_squares][i]);
            mpz_add(root->num_positions, root->num_positions, root->children[i]->num_positions);
        }
    }

    // effectively 1-indexed because of the MAX_BISHOPS_PSIDE value at index 0
    non_fixed_capturable_pieces[side][piece_type + 1] = 0;
    covered_sets[side][piece_type]                    = 0;
}

void* count_from_pieces(void* arg)
{
    create_tree_threading_struct ts = *(create_tree_threading_struct*)arg;

    int pawns[NUM_SIDES];
    int non_fixed_capturable_pieces[NUM_SIDES][NUM_PIECE_TYPES] = { 0 };
    int covered_sets[NUM_SIDES][NUM_PIECE_TYPES_LESS_KING]      = { 0 };
    int total_base_capturable_pieces[NUM_SIDES];
    for (int i = 0; i < NUM_SIDES; i++)
    {
        pawns[i] = ts.free_pawns[i] + ts.enpassant;
        // MAX_BISHOPS_PSIDE = 10 is the greatest number of any piece type, since
        // we can have either 2 bishops, knights or rooks and 8 promotions.
        // count_from_pieces_helper is recursive and for a side limits the number
        // of a certain piece by the number of the previous piece (to avoid
        // duplicate abstract sides), hence why we start at the maximum of any
        // piece type. Note that the type of each piece within
        // count_from_pieces_helper is abstract. We count the number of
        // permutations of piece types to the numbers of pieces at the end of the
        // count_from_pieces_helper chain
        non_fixed_capturable_pieces[i][0] = MAX_BISHOPS_PSIDE;
        total_base_capturable_pieces[i]   = ts.fixed_rooks[i];
    }
    int promotions[NUM_SIDES] = { 0 };
    count_from_pieces_helper(ts.root,
                             pawns,
                             ts.fixed_rooks,
                             non_fixed_capturable_pieces,
                             covered_sets,
                             total_base_capturable_pieces,
                             promotions,
                             ts.num_occupiable_squares,
                             1);

    return NULL;
}

// Level 4 and 5 account for rooks with castling rights (which we also call
// fixed rooks) and kings. We thread from this function
void* count_from_fixed_rooks_and_kings(void* arg)
{
    create_tree_threading_struct ts = *(create_tree_threading_struct*)arg;

    typedef void* (*fn)(void*);
    fn  next_call    = count_from_fixed_rooks_and_kings;
    int num_children = NUM_FIXED_ROOK_SCENARIOS; // 3
    if (ts.side)
    {
        next_call = count_from_pieces;
        if (!ts.enpassant && (ts.free_pawns[0] == ts.free_pawns[1]))
        {
            num_children = ts.fixed_rooks[0] + 1;
        }
    }

    ts.root->num_children = num_children;
    ts.root->children     = (position_node**)malloc(ts.root->num_children * sizeof(position_node*));
    for (int i = 0; i < ts.root->num_children; i++)
    {
        ts.root->children[i] = (position_node*)malloc(sizeof(position_node));
    }

    create_tree_threading_struct tstructs_out[NUM_FIXED_ROOK_SCENARIOS] = { 0 };
    for (int i = 0; i < num_children; i++)
    {
        tstructs_out[i].root          = ts.root->children[i];
        tstructs_out[i].free_pawns[0] = ts.free_pawns[0];
        tstructs_out[i].free_pawns[1] = ts.free_pawns[1];
        // Always copy over fr[0] even if 0
        tstructs_out[i].fixed_rooks[0] = ts.fixed_rooks[0];
        tstructs_out[i].enpassant      = ts.enpassant;
        tstructs_out[i].side           = 1;
    }

    pthread_t thread_ids[NUM_FIXED_ROOK_SCENARIOS];
    // CASE 1: no castling rights
    tstructs_out[NO_CASTLING_RIGHTS].num_occupiable_squares = ts.num_occupiable_squares;
    pthread_create(
        &thread_ids[NO_CASTLING_RIGHTS], NULL, next_call, &tstructs_out[NO_CASTLING_RIGHTS]);

    // CASE 2: castling rights on one side
    if (num_children > 1)
    {
        tstructs_out[CASTLING_RIGHTS_ONE_SIDE].fixed_rooks[ts.side] = 1;
        tstructs_out[CASTLING_RIGHTS_ONE_SIDE].num_occupiable_squares
            = ts.num_occupiable_squares - 2; // king + 1 fr
        pthread_create(&thread_ids[CASTLING_RIGHTS_ONE_SIDE],
                       NULL,
                       next_call,
                       &tstructs_out[CASTLING_RIGHTS_ONE_SIDE]);
    }

    // CASE 3: castling rights on both sides
    if (num_children > 2)
    {
        tstructs_out[CASTLING_RIGHTS_BOTH_SIDES].fixed_rooks[ts.side] = 2;
        tstructs_out[CASTLING_RIGHTS_BOTH_SIDES].num_occupiable_squares
            = ts.num_occupiable_squares - 3; // king + 2 fr
        pthread_create(&thread_ids[CASTLING_RIGHTS_BOTH_SIDES],
                       NULL,
                       next_call,
                       &tstructs_out[CASTLING_RIGHTS_BOTH_SIDES]);
    }

    for (int i = 0; i < num_children; i++)
    {
        pthread_join(thread_ids[i], NULL);
    }

    if (num_children > 1)
    {
        mpz_mul_ui(ts.root->children[CASTLING_RIGHTS_ONE_SIDE]->num_positions,
                   ts.root->children[CASTLING_RIGHTS_ONE_SIDE]->num_positions,
                   ONE_FIXED_ROOK_VARIATIONS); // 2; fixed rook can be either side
    }

    // Account for side to move, making sure to avoid fr[0] == fr[1]
    if (ts.side && !ts.enpassant && (ts.free_pawns[0] == ts.free_pawns[1]))
    {
        for (int i = 0; i < num_children - 1; i++)
        {
            mpz_mul_ui(ts.root->children[i]->num_positions, ts.root->children[i]->num_positions, 2);
        }
    }

    mpz_init(ts.root->num_positions);
    for (int i = 0; i < num_children; i++)
    {
        mpz_add(
            ts.root->num_positions, ts.root->num_positions, ts.root->children[i]->num_positions);
    }

    return NULL;
}

// The 2nd and 3rd levels represent free pawns (not pawns considered in setting
// up enpassant)
void count_from_free_pawns(position_node* root,
                           position_node* eerroot,
                           const bool     side,
                           const int      num_occupiable_squares,
                           const bool     enpassant,
                           const int      previous_free_pawns)
{
    root->num_children = 1 + NUM_PAWNS_PSIDE;
    if (enpassant)
    {
        --root->num_children;
    }
    else if (!side)
    {   
        // #side1Pawns >= #side0Pawns
        root->num_children = 1 + previous_free_pawns;
    }

    root->children = (position_node**)malloc(root->num_children * sizeof(position_node*));
    if (!side && eerroot != NULL)
    {
        for (int i = 0; i < root->num_children; i++)
        {
            root->children[i] = eerroot->children[previous_free_pawns]->children[i];
        }
    }
    else
    {
        for (int i = 0; i < root->num_children; i++)
        {
            root->children[i] = (position_node*)malloc(sizeof(position_node));
        }
        for (int i = 0; i < root->num_children; i++)
        {
            if (side)
            {
                count_from_free_pawns(
                    root->children[i], eerroot, 0, num_occupiable_squares - i, enpassant, i);
            }
            else
            {
                create_tree_threading_struct t = { 0 };
                t.root                         = root->children[i];
                t.free_pawns[1]                = previous_free_pawns;
                t.free_pawns[0]                = i;
                t.num_occupiable_squares       = num_occupiable_squares - i;
                t.enpassant                    = enpassant;

                (*count_from_fixed_rooks_and_kings)(&t);

                // Account for side to move
                if (!enpassant && i != previous_free_pawns)
                {
                    mpz_mul_ui(
                        root->children[i]->num_positions, root->children[i]->num_positions, 2);
                }
            }
            mpz_mul_ui(root->children[i]->num_positions,
                       root->children[i]->num_positions,
                       binomials[num_occupiable_squares - (eerroot != NULL)
                                 - (NUM_SIDES * BOARD_SIDE_LENGTH)][i]);
            // We subtract 16 squares from available squares because pawns can't be
            // placed on the 1st and 8th ranks. Note how we also remove one square if
            // we've been passed a non-null 'eeroot' because in this scenario we're
            // only allowed one adjacent enpassant pawn. If we allowed an opposition
            // pawn on this square we'd be double counting
        }
    }
    mpz_init(root->num_positions);
    for (int i = 0; i < root->num_children; i++)
    {
        mpz_add(root->num_positions, root->num_positions, root->children[i]->num_positions);
    }
}

// Level 1 (0 being the root) represents en-passant scenarios
void* count_from_enpassant(position_node* root)
{
    root->num_children = 3;
    root->children     = (position_node**)malloc(root->num_children * sizeof(position_node*));
    for (int i = 0; i < root->num_children; i++)
    {
        root->children[i] = (position_node*)malloc(sizeof(position_node));
    }

    // CASE 1: there's no en-passant square.
    // We determine the number of pawns in side1 (within count_from_free_pawns)
    // before side0 even though we normally count for side0 before side1. We do
    // this because if the en-passant square is a non-edge file ([b, g]) we have
    // 2 scenarios: there are either 1 or 2 en-passant-pawn rank-adjacent
    // squares occupied by side1 pawns. We need to consider both scenarios
    // separately to avoid counting duplicate positions. We handle this
    // temporarily unoccupiable adjacency ASAP by accounting for side1 before
    // side0 pawns after which the potentially empty adjacency would then be
    // considered once again occupiable
    count_from_free_pawns(
        root->children[NO_ENPASSANT], NULL, 1, NUM_SQUARES, false, NUM_PAWNS_PSIDE);

    // CASE 2: there's an en-passant square, and the en-passant pawn (side0's)
    // (a) has a left-rank-adjacent side1 pawn (in which case there could also
    // potentially be a right-rank-adjacent side1 pawn) or (b) is on an edge
    // file.
    //
    // Two squares are occupied by the en-passant pawn and the adjacent side1
    // pawn but there also can't be anything on the en-passant square or the
    // en-passant-pawn's starting square
    count_from_free_pawns(
        root->children[ENPASSANT_EDGE_AND_RIGHT], NULL, 1, NUM_SQUARES - 4, true, NUM_PAWNS_PSIDE);
    mpz_mul_ui(root->children[ENPASSANT_EDGE_AND_RIGHT]->num_positions,
               root->children[ENPASSANT_EDGE_AND_RIGHT]->num_positions,
               ENPASSANT_EDGE_AND_RIGHT_VARIATIONS); // 2 + 6

    // CASE 3: there's an en-passant square, and the en-passant pawn is not on
    // an edge file and has just one rank-adjacent side1 pawn which is
    // left-rank-adjacent
    count_from_free_pawns(root->children[ENPASSANT_LEFT_LESS_EDGE],
                          root->children[ENPASSANT_EDGE_AND_RIGHT],
                          1,
                          NUM_SQUARES - 4,
                          true,
                          NUM_PAWNS_PSIDE);
    mpz_mul_ui(root->children[ENPASSANT_LEFT_LESS_EDGE]->num_positions,
               root->children[ENPASSANT_LEFT_LESS_EDGE]->num_positions,
               ENPASSANT_LEFT_LESS_EDGE_VARIATIONS); // 6

    mpz_init(root->num_positions);
    for (int i = 0; i < root->num_children; i++)
    {
        mpz_add(root->num_positions, root->num_positions, root->children[i]->num_positions);
    }

    return NULL;
}

// Our sample space is a tree. The tree must contain at least every reachable
// position in chess, but will also contain unreachable positions. Each level of
// the tree represents a characteristic of a board, where boards with the exact
// same characteristics are represented by the same leaf. The tree doesn't store
// exact positions of course, that would be far too much memory. We abstract
// where exactly most pieces are. We include as much structure - where chessmen
// are or are bound within - as we need to generate positions. The tree is
// actually a DAG because certain nodes at the pawn level point to the same
// children. Ultimately the tree only has to be optimised to a point where it
// can be stored in a reasonable amount of memory and built within a reasonable
// amount of time. We build the tree in a DFS manner.
//
// Nodes in the tree are of type position_node which contains a GMP mpz_t for
// the size of the subtree rooted at the node, an int for the number of child
// nodes and an array of child nodes
void build_sample_space(position_node* root)
{
    count_from_enpassant(root);
    // Our abstracted position is a tuple (side0, side1, enpassant_square,
    // move). Note that side0 and side1 don't signify white and black. We
    // enforce that only side1 can have an en-passant capture available. When
    // there is no en-passant square and s0 == s1, we include just one of (s0,
    // s1, m) and (s1, s0, m). Therefore every position can be cast to w/b or b/w
    // and hence we multiply the size of the tree by 2 to represent the colour
    // permutations.
    mpz_mul_ui(root->num_positions, root->num_positions, 2);
}
