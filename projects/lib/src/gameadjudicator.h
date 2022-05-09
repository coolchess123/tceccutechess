/*
    This file is part of Cute Chess.
    Copyright (C) 2008-2018 Cute Chess authors

    Cute Chess is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Cute Chess is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Cute Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef GAMEADJUDICATOR_H
#define GAMEADJUDICATOR_H

#include "board/result.h"
namespace Chess { class Board; }
class MoveEvaluation;

/*!
 * \brief A class for adjudicating chess games
 *
 * The GameAdjudicator class can be used to adjudicate chess games when
 * the probability of a specific result is high enough.
 */
class LIB_EXPORT GameAdjudicator
{
	public:
		/*!
		 * Creates a new game adjudicator.
		 *
		 * By default all adjudication is disabled.
		 */
		GameAdjudicator();

		/*!
		 * Sets the draw adjudication threshold for each game.
		 *
		 * A game will be adjudicated as a draw if both players report
		 * a score that's within \a score centipawns from zero for at
		 * least \a moveCount consecutive moves, and at least
		 * \a moveNumber full moves have been played.
		 */
		void setDrawThreshold(int moveNumber, int moveCount, int score, int maxPieces=0, int maxPawns=0, bool reset=true);
		/*!
		 * Sets the resign adjudication threshold for each game.
		 *
		 * A game will be adjudicated as a loss for the player that
		 * made the last move if it reports a score that's at least
		 * \a score centipawns below zero for at least \a moveCount
		 * consecutive moves. If \a maxPieces>0, number of non-pawn pieces also needs to be no greater than \a maxPieces
		 * for the consecutive moves. Ditto with \a maxPawns for pawns. If \a reset is true, any reversible moves resets the count
		 * of consecutive moves to 0.
		 *
		 */
		void setResignThreshold(int moveCount, int score);
		/*!
		 * Limits the number of moves playable in a game.
		 *
		 * A game will be adjudicated as a draw when the number of
		 * moves played exceeds the limit given by \a moveCount.
		 * The limit is not in action if set to zero.
		 */
		void setMaximumGameLength(int moveCount);
		/*!
		 * Sets tablebase adjudication to \a enable.
		 *
		 * If \a enable is true then games are adjudicated if the
		 * latest position is found in the tablebases.
		 */
		void setTablebaseAdjudication(bool enable, bool drawOnly);
		/*!
		 * Sets TCEC adjudication to \a enable.
		 *
		 * If \a enable is true then the resign rule requires both player
		 * evaluations to be below the setResignThreshold score.
		 */
		void setTcecAdjudication(bool enable);
		/*!
		 * Adds a new move evaluation to the adjudicator.
		 *
		 * \a board should be at the position that follows the move.
		 * \a eval should be the evaluation of the move.
		 *
		 * result() can be called after this function to find out if
		 * the game should be adjudicated.
		 */
		void addEval(const Chess::Board* board, const MoveEvaluation& eval);
		/*! Sets draw move count to 0. */
		void resetDrawMoveCount();
		/*!
		 * Returns the adjudication result.
		 *
		 * This function returns the expected result of the game. If the
		 * game can't be adjudicated yet, a null result is returned.
		 */
		Chess::Result result() const;
		/*!
		 * Returns the number of plies left until draw rule adjudication.
		 */
		int drawClock(const Chess::Board* board, const MoveEvaluation& eval) const;
		/*!
		 * Returns the number of plies left until resign rule adjudication.
		 */
		int resignClock(const Chess::Board* board, const MoveEvaluation& eval) const;
		/*!
		 * Returns true if the draw rules count resets on irreversible moves
		 */
		bool resets() const;

	private:
		int m_drawMoveNum;
		int m_drawMoveCount;
		int m_drawScore;
		int m_drawScoreCount;
		int m_resignMoveCount;
		int m_resignScore;
		int m_resignScoreCount[2];
		int m_maxGameLength;
		bool m_tbEnabled;
		bool m_tbDrawOnly;
		Chess::Result m_result;
		int m_resignWinnerScoreCount[2];
		bool m_tcecAdjudication;
		int m_maxPawns;
		int m_maxPieces;
		bool m_reset;
};

#endif // GAMEADJUDICATOR_H
