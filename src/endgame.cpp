/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2014 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>

#include "bitboard.h"
#include "bitcount.h"
#include "endgame.h"
#include "movegen.h"

using std::string;

namespace {

  // Table used to drive the king towards the edge of the board
  // in KX vs K and KQ vs KR endgames.
  const int PushToEdges[SQUARE_NB] = {
    100, 90, 80, 70, 70, 80, 90, 100,
     90, 70, 60, 50, 50, 60, 70,  90,
     80, 60, 40, 30, 30, 40, 60,  80,
     70, 50, 30, 20, 20, 30, 50,  70,
     70, 50, 30, 20, 20, 30, 50,  70,
     80, 60, 40, 30, 30, 40, 60,  80,
     90, 70, 60, 50, 50, 60, 70,  90,
    100, 90, 80, 70, 70, 80, 90, 100,
  };

  // Table used to drive the king towards a corner square of the
  // right color in KBN vs K endgames.
  const int PushToCorners[SQUARE_NB] = {
    200, 190, 180, 170, 160, 150, 140, 130,
    190, 180, 170, 160, 150, 140, 130, 140,
    180, 170, 155, 140, 140, 125, 140, 150,
    170, 160, 140, 120, 110, 140, 150, 160,
    160, 150, 140, 110, 120, 140, 160, 170,
    150, 140, 125, 140, 140, 155, 170, 180,
    140, 130, 140, 150, 160, 170, 180, 190,
    130, 140, 150, 160, 170, 180, 190, 200
  };

  // Tables used to drive a piece towards or away from another piece
  const int PushClose[8] = { 0, 0, 100, 80, 60, 40, 20, 10 };
  const int PushAway [8] = { 0, 5, 20, 40, 60, 80, 90, 100 };

#ifndef NDEBUG
  bool verify_material(const Position& pos, Color color, Value nonPawnMaterial, int numPawns) {
    return pos.non_pawn_material(color) == nonPawnMaterial && pos.count<PAWN>(color) == numPawns;
  }
#endif

  // Map the square as if strongSide is white and strongSide's only pawn
  // is on the left half of the board.
  Square normalize(const Position& pos, Color strongSide, Square square) {

    assert(pos.count<PAWN>(strongSide) == 1);

    if (file_of(pos.list<PAWN>(strongSide)[0]) >= FILE_E)
        square = Square(square ^ 7); // Mirror SQ_H1 -> SQ_A1

    if (strongSide == BLACK)
        square = ~square;

    return square;
  }

  // Get the material key of Position out of the given endgame key code
  // like "KBPKN". The trick here is to first forge an ad-hoc FEN string
  // and then let a Position object do the work for us.
  Key key(const string& code, Color color) {

    assert(code.length() > 0 && code.length() < 8);
    assert(code[0] == 'K');

    string sides[] = { code.substr(code.find('K', 1)),      // Weak
                       code.substr(0, code.find('K', 1)) }; // Strong

    std::transform(sides[color].begin(), sides[color].end(), sides[color].begin(), tolower);

    string fenString =  sides[0] + char(8 - sides[0].length() + '0') + "/8/8/8/8/8/8/"
                      + sides[1] + char(8 - sides[1].length() + '0') + " w - - 0 10";

    return Position(fenString, false, NULL).material_key();
  }

  template<typename M>
  void delete_endgame(const typename M::value_type& p) { delete p.second; }

} // namespace


/// Endgames members definitions

Endgames::Endgames() {

  add<KPK>("KPK");
  add<KNNK>("KNNK");
  add<KBNK>("KBNK");
  add<KRKP>("KRKP");
  add<KRKB>("KRKB");
  add<KRKN>("KRKN");
  add<KQKP>("KQKP");
  add<KQKR>("KQKR");

  add<KNPK>("KNPK");
  add<KNPKB>("KNPKB");
  add<KRPKR>("KRPKR");
  add<KRPKB>("KRPKB");
  add<KBPKB>("KBPKB");
  add<KBPKN>("KBPKN");
  add<KBPPKB>("KBPPKB");
  add<KRPPKRP>("KRPPKRP");
}

Endgames::~Endgames() {

  for_each(m1.begin(), m1.end(), delete_endgame<M1>);
  for_each(m2.begin(), m2.end(), delete_endgame<M2>);
}

template<EndgameType E>
void Endgames::add(const string& code) {

  map((Endgame<E>*)0)[key(code, WHITE)] = new Endgame<E>(WHITE);
  map((Endgame<E>*)0)[key(code, BLACK)] = new Endgame<E>(BLACK);
}


/// Mate with KX vs K. This function is used to evaluate positions with
/// king and plenty of material vs a lone king. It simply gives the
/// attacking side a bonus for driving the defending king towards the edge
/// of the board, and for keeping the distance between the two kings small.
template<>
Value Endgame<KXK>::operator()(const Position& pos) const {

  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));
  assert(!pos.checkers()); // Eval is never called when in check

  // Stalemate detection with lone king
  if (pos.side_to_move() == weakSide && !MoveList<LEGAL>(pos).size())
      return VALUE_DRAW;

  Square winnerKingSquare = pos.king_square(strongSide);
  Square loserKingSquare = pos.king_square(weakSide);

  Value result =  pos.non_pawn_material(strongSide)
                + pos.count<PAWN>(strongSide) * PawnValueEg
                + PushToEdges[loserKingSquare]
                + PushClose[distance(winnerKingSquare, loserKingSquare)];

  if (   pos.count<QUEEN>(strongSide)
      || pos.count<ROOK>(strongSide)
      ||(pos.count<BISHOP>(strongSide) && pos.count<KNIGHT>(strongSide))
      || pos.bishop_pair(strongSide))
      result += VALUE_KNOWN_WIN;

  return strongSide == pos.side_to_move() ? result : -result;
}


/// Mate with KBN vs K. This is similar to KX vs K, but we have to drive the
/// defending king towards a corner square of the right color.
template<>
Value Endgame<KBNK>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, KnightValueMg + BishopValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  Square winnerKingSquare = pos.king_square(strongSide);
  Square loserKingSquare = pos.king_square(weakSide);
  Square bishopSquare = pos.list<BISHOP>(strongSide)[0];

  // kbnk_mate_table() tries to drive toward corners A1 or H8. If we have a
  // bishop that cannot reach the above squares, we flip the kings in order
  // to drive the enemy toward corners A8 or H1.
  if (opposite_colors(bishopSquare, SQ_A1))
  {
      winnerKingSquare = ~winnerKingSquare;
      loserKingSquare  = ~loserKingSquare;
  }

  Value result =  VALUE_KNOWN_WIN
                + PushClose[distance(winnerKingSquare, loserKingSquare)]
                + PushToCorners[loserKingSquare];

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KP vs K. This endgame is evaluated with the help of a bitbase.
template<>
Value Endgame<KPK>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, VALUE_ZERO, 1));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square whiteKingSquare = normalize(pos, strongSide, pos.king_square(strongSide));
  Square blackKingSquare = normalize(pos, strongSide, pos.king_square(weakSide));
  Square pawnSquare  = normalize(pos, strongSide, pos.list<PAWN>(strongSide)[0]);

  Color us = strongSide == pos.side_to_move() ? WHITE : BLACK;

  if (!Bitbases::probe_kpk(whiteKingSquare, pawnSquare, blackKingSquare, us))
      return VALUE_DRAW;

  Value result = VALUE_KNOWN_WIN + PawnValueEg + Value(rank_of(pawnSquare));

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KR vs KP. This is a somewhat tricky endgame to evaluate precisely without
/// a bitbase. The function below returns drawish scores when the pawn is
/// far advanced with support of the king, while the attacking king is far
/// away.
template<>
Value Endgame<KRKP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square whiteKingSquare = relative_square(strongSide, pos.king_square(strongSide));
  Square blackKingSquare = relative_square(strongSide, pos.king_square(weakSide));
  Square rookSquare  = relative_square(strongSide, pos.list<ROOK>(strongSide)[0]);
  Square pawnSquare  = relative_square(strongSide, pos.list<PAWN>(weakSide)[0]);

  Square queeningSquare = make_square(file_of(pawnSquare), RANK_1);
  Value result;

  // If the stronger side's king is in front of the pawn, it's a win
  if (whiteKingSquare < pawnSquare && file_of(whiteKingSquare) == file_of(pawnSquare))
      result = RookValueEg - distance(whiteKingSquare, pawnSquare);

  // If the weaker side's king is too far from the pawn and the rook,
  // it's a win.
  else if (   distance(blackKingSquare, pawnSquare) >= 3 + (pos.side_to_move() == weakSide)
           && distance(blackKingSquare, rookSquare) >= 3)
      result = RookValueEg - distance(whiteKingSquare, pawnSquare);

  // If the pawn is far advanced and supported by the defending king,
  // the position is drawish
  else if (   rank_of(blackKingSquare) <= RANK_3
           && distance(blackKingSquare, pawnSquare) == 1
           && rank_of(whiteKingSquare) >= RANK_4
           && distance(whiteKingSquare, pawnSquare) > 2 + (pos.side_to_move() == strongSide))
      result = Value(80) - 8 * distance(whiteKingSquare, pawnSquare);

  else
      result =  Value(200) - 8 * (  distance(whiteKingSquare, pawnSquare + DELTA_S)
                                  - distance(blackKingSquare, pawnSquare + DELTA_S)
                                  - distance(pawnSquare, queeningSquare));

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KR vs KB. This is very simple, and always returns drawish scores.  The
/// score is slightly bigger when the defending king is close to the edge.
template<>
Value Endgame<KRKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, BishopValueMg, 0));

  Value result = Value(PushToEdges[pos.king_square(weakSide)]);
  return strongSide == pos.side_to_move() ? result : -result;
}


/// KR vs KN. The attacking side has slightly better winning chances than
/// in KR vs KB, particularly if the king and the knight are far apart.
template<>
Value Endgame<KRKN>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, KnightValueMg, 0));

  Square blackKingSquare = pos.king_square(weakSide);
  Square knightSquare = pos.list<KNIGHT>(weakSide)[0];
  Value result = Value(PushToEdges[blackKingSquare] + PushAway[distance(blackKingSquare, knightSquare)]);
  return strongSide == pos.side_to_move() ? result : -result;
}


/// KQ vs KP. In general, this is a win for the stronger side, but there are a
/// few important exceptions. A pawn on 7th rank and on the A,C,F or H files
/// with a king positioned next to it can be a draw, so in that case, we only
/// use the distance between the kings.
template<>
Value Endgame<KQKP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square winnerKingSquare = pos.king_square(strongSide);
  Square loserKingSquare = pos.king_square(weakSide);
  Square pawnSquare = pos.list<PAWN>(weakSide)[0];

  Value result = Value(PushClose[distance(winnerKingSquare, loserKingSquare)]);

  if (   relative_rank(weakSide, pawnSquare) != RANK_7
      || distance(loserKingSquare, pawnSquare) != 1
      || !((FileABB | FileCBB | FileFBB | FileHBB) & pawnSquare))
      result += QueenValueEg - PawnValueEg;

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KQ vs KR.  This is almost identical to KX vs K:  We give the attacking
/// king a bonus for having the kings close together, and for forcing the
/// defending king towards the edge. If we also take care to avoid null move for
/// the defending side in the search, this is usually sufficient to win KQ vs KR.
template<>
Value Endgame<KQKR>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(verify_material(pos, weakSide, RookValueMg, 0));

  Square winnerKingSquare = pos.king_square(strongSide);
  Square loserKingSquare = pos.king_square(weakSide);

  Value result =  QueenValueEg
                - RookValueEg
                + PushToEdges[loserKingSquare]
                + PushClose[distance(winnerKingSquare, loserKingSquare)];

  return strongSide == pos.side_to_move() ? result : -result;
}


/// Some cases of trivial draws
template<> Value Endgame<KNNK>::operator()(const Position&) const { return VALUE_DRAW; }


/// KB and one or more pawns vs K. It checks for draws with rook pawns and
/// a bishop of the wrong color. If such a draw is detected, SCALE_FACTOR_DRAW
/// is returned. If not, the return value is SCALE_FACTOR_NONE, i.e. no scaling
/// will be used.
template<>
ScaleFactor Endgame<KBPsK>::operator()(const Position& pos) const {

  assert(pos.non_pawn_material(strongSide) == BishopValueMg);
  assert(pos.count<PAWN>(strongSide) >= 1);

  // No assertions about the material of weakSide, because we want draws to
  // be detected even when the weaker side has some pawns.

  Bitboard pawns = pos.pieces(strongSide, PAWN);
  File pawnFile = file_of(pos.list<PAWN>(strongSide)[0]);

  // All pawns are on a single rook file ?
  if (    (pawnFile == FILE_A || pawnFile == FILE_H)
      && !(pawns & ~file_bb(pawnFile)))
  {
      Square bishopSquare = pos.list<BISHOP>(strongSide)[0];
      Square queeningSquare = relative_square(strongSide, make_square(pawnFile, RANK_8));
      Square kingSquare = pos.king_square(weakSide);

      if (   opposite_colors(queeningSquare, bishopSquare)
          && distance(queeningSquare, kingSquare) <= 1)
          return SCALE_FACTOR_DRAW;
  }

  // If all the pawns are on the same B or G file, then it's potentially a draw
  if (    (pawnFile == FILE_B || pawnFile == FILE_G)
      && !(pos.pieces(PAWN) & ~file_bb(pawnFile))
      && pos.non_pawn_material(weakSide) == 0
      && pos.count<PAWN>(weakSide) >= 1)
  {
      // Get weakSide pawn that is closest to the home rank
      Square weakPawnSquare = backmost_sq(weakSide, pos.pieces(weakSide, PAWN));

      Square strongKingSquare = pos.king_square(strongSide);
      Square weakKingSquare = pos.king_square(weakSide);
      Square bishopSquare = pos.list<BISHOP>(strongSide)[0];

      // There's potential for a draw if our pawn is blocked on the 7th rank,
      // the bishop cannot attack it or they only have one pawn left
      if (   relative_rank(strongSide, weakPawnSquare) == RANK_7
          && (pos.pieces(strongSide, PAWN) & (weakPawnSquare + pawn_push(weakSide)))
          && (opposite_colors(bishopSquare, weakPawnSquare) || pos.count<PAWN>(strongSide) == 1))
      {
          int strongKingDistance = distance(weakPawnSquare, strongKingSquare);
          int weakKingDistance = distance(weakPawnSquare, weakKingSquare);

          // It's a draw if the weak king is on its back two ranks, within 2
          // squares of the blocking pawn and the strong king is not
          // closer. (I think this rule only fails in practically
          // unreachable positions such as 5k1K/6p1/6P1/8/8/3B4/8/8 w
          // and positions where qsearch will immediately correct the
          // problem such as 8/4k1p1/6P1/1K6/3B4/8/8/8 w)
          if (   relative_rank(strongSide, weakKingSquare) >= RANK_7
              && weakKingDistance <= 2
              && weakKingDistance <= strongKingDistance)
              return SCALE_FACTOR_DRAW;
      }
  }

  return SCALE_FACTOR_NONE;
}


/// KQ vs KR and one or more pawns. It tests for fortress draws with a rook on
/// the third rank defended by a pawn.
template<>
ScaleFactor Endgame<KQKRPs>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(pos.count<ROOK>(weakSide) == 1);
  assert(pos.count<PAWN>(weakSide) >= 1);

  Square kingSquare = pos.king_square(weakSide);
  Square rookSquare = pos.list<ROOK>(weakSide)[0];

  if (    relative_rank(weakSide, kingSquare) <= RANK_2
      &&  relative_rank(weakSide, pos.king_square(strongSide)) >= RANK_4
      &&  relative_rank(weakSide, rookSquare) == RANK_3
      && (  pos.pieces(weakSide, PAWN)
          & pos.attacks_from<KING>(kingSquare)
          & pos.attacks_from<PAWN>(rookSquare, strongSide)))
          return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KRP vs KR. This function knows a handful of the most important classes of
/// drawn positions, but is far from perfect. It would probably be a good idea
/// to add more knowledge in the future.
///
/// It would also be nice to rewrite the actual code for this function,
/// which is mostly copied from Glaurung 1.x, and isn't very pretty.
template<>
ScaleFactor Endgame<KRPKR>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide,   RookValueMg, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square whiteKingSquare = normalize(pos, strongSide, pos.king_square(strongSide));
  Square blackKingSquare = normalize(pos, strongSide, pos.king_square(weakSide));
  Square whiteRookSquare = normalize(pos, strongSide, pos.list<ROOK>(strongSide)[0]);
  Square whitePawnSquare = normalize(pos, strongSide, pos.list<PAWN>(strongSide)[0]);
  Square blackRookSquare = normalize(pos, strongSide, pos.list<ROOK>(weakSide)[0]);

  File file = file_of(whitePawnSquare);
  Rank rank = rank_of(whitePawnSquare);
  Square queeningSquare = make_square(file, RANK_8);
  int tempo = (pos.side_to_move() == strongSide);

  // If the pawn is not too far advanced and the defending king defends the
  // queening square, use the third-rank defence.
  if (   rank <= RANK_5
      && distance(blackKingSquare, queeningSquare) <= 1
      && whiteKingSquare <= SQ_H5
      && (rank_of(blackRookSquare) == RANK_6 || (rank <= RANK_3 && rank_of(whiteRookSquare) != RANK_6)))
      return SCALE_FACTOR_DRAW;

  // The defending side saves a draw by checking from behind in case the pawn
  // has advanced to the 6th rank with the king behind.
  if (   rank == RANK_6
      && distance(blackKingSquare, queeningSquare) <= 1
      && rank_of(whiteKingSquare) + tempo <= RANK_6
      && (rank_of(blackRookSquare) == RANK_1 || (!tempo && distance(file_of(blackRookSquare), file) >= 3)))
      return SCALE_FACTOR_DRAW;

  if (   rank >= RANK_6
      && blackKingSquare == queeningSquare
      && rank_of(blackRookSquare) == RANK_1
      && (!tempo || distance(whiteKingSquare, whitePawnSquare) >= 2))
      return SCALE_FACTOR_DRAW;

  // White pawn on a7 and rook on a8 is a draw if black's king is on g7 or h7
  // and the black rook is behind the pawn.
  if (   whitePawnSquare == SQ_A7
      && whiteRookSquare == SQ_A8
      && (blackKingSquare == SQ_H7 || blackKingSquare == SQ_G7)
      && file_of(blackRookSquare) == FILE_A
      && (rank_of(blackRookSquare) <= RANK_3 || file_of(whiteKingSquare) >= FILE_D || rank_of(whiteKingSquare) <= RANK_5))
      return SCALE_FACTOR_DRAW;

  // If the defending king blocks the pawn and the attacking king is too far
  // away, it's a draw.
  if (   rank <= RANK_5
      && blackKingSquare == whitePawnSquare + DELTA_N
      && distance(whiteKingSquare, whitePawnSquare) - tempo >= 2
      && distance(whiteKingSquare, blackRookSquare) - tempo >= 2)
      return SCALE_FACTOR_DRAW;

  // Pawn on the 7th rank supported by the rook from behind usually wins if the
  // attacking king is closer to the queening square than the defending king,
  // and the defending king cannot gain tempi by threatening the attacking rook.
  if (   rank == RANK_7
      && file != FILE_A
      && file_of(whiteRookSquare) == file
      && whiteRookSquare != queeningSquare
      && (distance(whiteKingSquare, queeningSquare) < distance(blackKingSquare, queeningSquare) - 2 + tempo)
      && (distance(whiteKingSquare, queeningSquare) < distance(blackKingSquare, whiteRookSquare) + tempo))
      return ScaleFactor(SCALE_FACTOR_MAX - 2 * distance(whiteKingSquare, queeningSquare));

  // Similar to the above, but with the pawn further back
  if (   file != FILE_A
      && file_of(whiteRookSquare) == file
      && whiteRookSquare < whitePawnSquare
      && (distance(whiteKingSquare, queeningSquare) < distance(blackKingSquare, queeningSquare) - 2 + tempo)
      && (distance(whiteKingSquare, whitePawnSquare + DELTA_N) < distance(blackKingSquare, whitePawnSquare + DELTA_N) - 2 + tempo)
      && (  distance(blackKingSquare, whiteRookSquare) + tempo >= 3
          || (    distance(whiteKingSquare, queeningSquare) < distance(blackKingSquare, whiteRookSquare) + tempo
              && (distance(whiteKingSquare, whitePawnSquare + DELTA_N) < distance(blackKingSquare, whiteRookSquare) + tempo))))
      return ScaleFactor(  SCALE_FACTOR_MAX
                         - 8 * distance(whitePawnSquare, queeningSquare)
                         - 2 * distance(whiteKingSquare, queeningSquare));

  // If the pawn is not far advanced and the defending king is somewhere in
  // the pawn's path, it's probably a draw.
  if (rank <= RANK_4 && blackKingSquare > whitePawnSquare)
  {
      if (file_of(blackKingSquare) == file_of(whitePawnSquare))
          return ScaleFactor(10);
      if (   distance<File>(blackKingSquare, whitePawnSquare) == 1
          && distance(whiteKingSquare, blackKingSquare) > 2)
          return ScaleFactor(24 - 2 * distance(whiteKingSquare, blackKingSquare));
  }
  return SCALE_FACTOR_NONE;
}

template<>
ScaleFactor Endgame<KRPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide, BishopValueMg, 0));

  // Test for a rook pawn
  if (pos.pieces(PAWN) & (FileABB | FileHBB))
  {
      Square kingSquare = pos.king_square(weakSide);
      Square bishopSquare = pos.list<BISHOP>(weakSide)[0];
      Square pawnSquare = pos.list<PAWN>(strongSide)[0];
      Rank rank = relative_rank(strongSide, pawnSquare);
      Square push = pawn_push(strongSide);

      // If the pawn is on the 5th rank and the pawn (currently) is on
      // the same color square as the bishop then there is a chance of
      // a fortress. Depending on the king position give a moderate
      // reduction or a stronger one if the defending king is near the
      // corner but not trapped there.
      if (rank == RANK_5 && !opposite_colors(bishopSquare, pawnSquare))
      {
          int kingDistance = distance(pawnSquare + 3 * push, kingSquare);

          if (kingDistance <= 2 && !(kingDistance == 0 && kingSquare == pos.king_square(strongSide) + 2 * push))
              return ScaleFactor(24);
          else
              return ScaleFactor(48);
      }

      // When the pawn has moved to the 6th rank we can be fairly sure
      // it's drawn if the bishop attacks the square in front of the
      // pawn from a reasonable distance and the defending king is near
      // the corner
      if (   rank == RANK_6
          && distance(pawnSquare + 2 * push, kingSquare) <= 1
          && (PseudoAttacks[BISHOP][bishopSquare] & (pawnSquare + push))
          && distance<File>(bishopSquare, pawnSquare) >= 2)
          return ScaleFactor(8);
  }

  return SCALE_FACTOR_NONE;
}

/// KRPP vs KRP. There is just a single rule: if the stronger side has no passed
/// pawns and the defending king is actively placed, the position is drawish.
template<>
ScaleFactor Endgame<KRPPKRP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 2));
  assert(verify_material(pos, weakSide,   RookValueMg, 1));

  Square whitePawnSquare1 = pos.list<PAWN>(strongSide)[0];
  Square whitePawnSquare2 = pos.list<PAWN>(strongSide)[1];
  Square blackKingSquare = pos.king_square(weakSide);

  // Does the stronger side have a passed pawn?
  if (pos.pawn_passed(strongSide, whitePawnSquare1) || pos.pawn_passed(strongSide, whitePawnSquare2))
      return SCALE_FACTOR_NONE;

  Rank rank = std::max(relative_rank(strongSide, whitePawnSquare1), relative_rank(strongSide, whitePawnSquare2));

  if (   distance<File>(blackKingSquare, whitePawnSquare1) <= 1
      && distance<File>(blackKingSquare, whitePawnSquare2) <= 1
      && relative_rank(strongSide, blackKingSquare) > rank)
  {
      switch (rank) {
      case RANK_2: return ScaleFactor(10);
      case RANK_3: return ScaleFactor(10);
      case RANK_4: return ScaleFactor(15);
      case RANK_5: return ScaleFactor(20);
      case RANK_6: return ScaleFactor(40);
      default: assert(false);
      }
  }
  return SCALE_FACTOR_NONE;
}


/// K and two or more pawns vs K. There is just a single rule here: If all pawns
/// are on the same rook file and are blocked by the defending king, it's a draw.
template<>
ScaleFactor Endgame<KPsK>::operator()(const Position& pos) const {

  assert(pos.non_pawn_material(strongSide) == VALUE_ZERO);
  assert(pos.count<PAWN>(strongSide) >= 2);
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  Square kingSquare = pos.king_square(weakSide);
  Bitboard pawns = pos.pieces(strongSide, PAWN);
  Square pawnSquare = pos.list<PAWN>(strongSide)[0];

  // If all pawns are ahead of the king, on a single rook file and
  // the king is within one file of the pawns, it's a draw.
  if (   !(pawns & ~in_front_bb(weakSide, rank_of(kingSquare)))
      && !((pawns & ~FileABB) && (pawns & ~FileHBB))
      &&  distance<File>(kingSquare, pawnSquare) <= 1)
      return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KBP vs KB. There are two rules: if the defending king is somewhere along the
/// path of the pawn, and the square of the king is not of the same color as the
/// stronger side's bishop, it's a draw. If the two bishops have opposite color,
/// it's almost always a draw.
template<>
ScaleFactor Endgame<KBPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square pawnSquare = pos.list<PAWN>(strongSide)[0];
  Square strongBishopSquare = pos.list<BISHOP>(strongSide)[0];
  Square weakBishopSquare = pos.list<BISHOP>(weakSide)[0];
  Square weakKingSquare = pos.king_square(weakSide);

  // Case 1: Defending king blocks the pawn, and cannot be driven away
  if (   file_of(weakKingSquare) == file_of(pawnSquare)
      && relative_rank(strongSide, pawnSquare) < relative_rank(strongSide, weakKingSquare)
      && (   opposite_colors(weakKingSquare, strongBishopSquare)
          || relative_rank(strongSide, weakKingSquare) <= RANK_6))
      return SCALE_FACTOR_DRAW;

  // Case 2: Opposite colored bishops
  if (opposite_colors(strongBishopSquare, weakBishopSquare))
  {
      // We assume that the position is drawn in the following three situations:
      //
      //   a. The pawn is on rank 5 or further back.
      //   b. The defending king is somewhere in the pawn's path.
      //   c. The defending bishop attacks some square along the pawn's path,
      //      and is at least three squares away from the pawn.
      //
      // These rules are probably not perfect, but in practice they work
      // reasonably well.

      if (relative_rank(strongSide, pawnSquare) <= RANK_5)
          return SCALE_FACTOR_DRAW;
      else
      {
          Bitboard path = forward_bb(strongSide, pawnSquare);

          if (path & pos.pieces(weakSide, KING))
              return SCALE_FACTOR_DRAW;

          if (  (pos.attacks_from<BISHOP>(weakBishopSquare) & path)
              && distance(weakBishopSquare, pawnSquare) >= 3)
              return SCALE_FACTOR_DRAW;
      }
  }
  return SCALE_FACTOR_NONE;
}


/// KBPP vs KB. It detects a few basic draws with opposite-colored bishops
template<>
ScaleFactor Endgame<KBPPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 2));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square whiteBishopSquare = pos.list<BISHOP>(strongSide)[0];
  Square blackBishopSquare = pos.list<BISHOP>(weakSide)[0];

  if (!opposite_colors(whiteBishopSquare, blackBishopSquare))
      return SCALE_FACTOR_NONE;

  Square kingSquare = pos.king_square(weakSide);
  Square pawnSquare1 = pos.list<PAWN>(strongSide)[0];
  Square pawnSquare2 = pos.list<PAWN>(strongSide)[1];
  Rank rank1 = rank_of(pawnSquare1);
  Rank rank2 = rank_of(pawnSquare2);
  Square blockSquare1, blockSquare2;

  if (relative_rank(strongSide, pawnSquare1) > relative_rank(strongSide, pawnSquare2))
  {
      blockSquare1 = pawnSquare1 + pawn_push(strongSide);
      blockSquare2 = make_square(file_of(pawnSquare2), rank_of(pawnSquare1));
  }
  else
  {
      blockSquare1 = pawnSquare2 + pawn_push(strongSide);
      blockSquare2 = make_square(file_of(pawnSquare1), rank_of(pawnSquare2));
  }

  switch (distance<File>(pawnSquare1, pawnSquare2))
  {
  case 0:
    // Both pawns are on the same file. It's an easy draw if the defender firmly
    // controls some square in the frontmost pawn's path.
    if (   file_of(kingSquare) == file_of(blockSquare1)
        && relative_rank(strongSide, kingSquare) >= relative_rank(strongSide, blockSquare1)
        && opposite_colors(kingSquare, whiteBishopSquare))
        return SCALE_FACTOR_DRAW;
    else
        return SCALE_FACTOR_NONE;

  case 1:
    // Pawns on adjacent files. It's a draw if the defender firmly controls the
    // square in front of the frontmost pawn's path, and the square diagonally
    // behind this square on the file of the other pawn.
    if (   kingSquare == blockSquare1
        && opposite_colors(kingSquare, whiteBishopSquare)
        && (   blackBishopSquare == blockSquare2
            || (pos.attacks_from<BISHOP>(blockSquare2) & pos.pieces(weakSide, BISHOP))
            || distance(rank1, rank2) >= 2))
        return SCALE_FACTOR_DRAW;

    else if (   kingSquare == blockSquare2
             && opposite_colors(kingSquare, whiteBishopSquare)
             && (   blackBishopSquare == blockSquare1
                 || (pos.attacks_from<BISHOP>(blockSquare1) & pos.pieces(weakSide, BISHOP))))
        return SCALE_FACTOR_DRAW;
    else
        return SCALE_FACTOR_NONE;

  default:
    // The pawns are not on the same file or adjacent files. No scaling.
    return SCALE_FACTOR_NONE;
  }
}


/// KBP vs KN. There is a single rule: If the defending king is somewhere along
/// the path of the pawn, and the square of the king is not of the same color as
/// the stronger side's bishop, it's a draw.
template<>
ScaleFactor Endgame<KBPKN>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide, KnightValueMg, 0));

  Square pawnSquare = pos.list<PAWN>(strongSide)[0];
  Square strongBishopSquare = pos.list<BISHOP>(strongSide)[0];
  Square weakKingSquare = pos.king_square(weakSide);

  if (   file_of(weakKingSquare) == file_of(pawnSquare)
      && relative_rank(strongSide, pawnSquare) < relative_rank(strongSide, weakKingSquare)
      && (   opposite_colors(weakKingSquare, strongBishopSquare)
          || relative_rank(strongSide, weakKingSquare) <= RANK_6))
      return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KNP vs K. There is a single rule: if the pawn is a rook pawn on the 7th rank
/// and the defending king prevents the pawn from advancing, the position is drawn.
template<>
ScaleFactor Endgame<KNPK>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, KnightValueMg, 1));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square pawnSquare = normalize(pos, strongSide, pos.list<PAWN>(strongSide)[0]);
  Square weakKingSquare = normalize(pos, strongSide, pos.king_square(weakSide));

  if (pawnSquare == SQ_A7 && distance(SQ_A8, weakKingSquare) <= 1)
      return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KNP vs KB. If knight can block bishop from taking pawn, it's a win.
/// Otherwise the position is drawn.
template<>
ScaleFactor Endgame<KNPKB>::operator()(const Position& pos) const {

  Square pawnSquare = pos.list<PAWN>(strongSide)[0];
  Square bishopSquare = pos.list<BISHOP>(weakSide)[0];
  Square weakKingSquare = pos.king_square(weakSide);

  // King needs to get close to promoting pawn to prevent knight from blocking.
  // Rules for this are very tricky, so just approximate.
  if (forward_bb(strongSide, pawnSquare) & pos.attacks_from<BISHOP>(bishopSquare))
      return ScaleFactor(distance(weakKingSquare, pawnSquare));

  return SCALE_FACTOR_NONE;
}


/// KP vs KP. This is done by removing the weakest side's pawn and probing the
/// KP vs K bitbase: If the weakest side has a draw without the pawn, it probably
/// has at least a draw with the pawn as well. The exception is when the stronger
/// side's pawn is far advanced and not on a rook file; in this case it is often
/// possible to win (e.g. 8/4k3/3p4/3P4/6K1/8/8/8 w - - 0 1).
template<>
ScaleFactor Endgame<KPKP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, VALUE_ZERO, 1));
  assert(verify_material(pos, weakSide,   VALUE_ZERO, 1));

  // Assume strongSide is white and the pawn is on files A-D
  Square whiteKingSquare = normalize(pos, strongSide, pos.king_square(strongSide));
  Square blackKingSquare = normalize(pos, strongSide, pos.king_square(weakSide));
  Square pawnSquare  = normalize(pos, strongSide, pos.list<PAWN>(strongSide)[0]);

  Color us = strongSide == pos.side_to_move() ? WHITE : BLACK;

  // If the pawn has advanced to the fifth rank or further, and is not a
  // rook pawn, it's too dangerous to assume that it's at least a draw.
  if (rank_of(pawnSquare) >= RANK_5 && file_of(pawnSquare) != FILE_A)
      return SCALE_FACTOR_NONE;

  // Probe the KPK bitbase with the weakest side's pawn removed. If it's a draw,
  // it's probably at least a draw even with the pawn.
  return Bitbases::probe_kpk(whiteKingSquare, pawnSquare, blackKingSquare, us) ? SCALE_FACTOR_NONE : SCALE_FACTOR_DRAW;
}
