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


#include "roundrobintournament.h"
#include <algorithm>

RoundRobinTournament::RoundRobinTournament(GameManager* gameManager,
					   EngineManager* engineManager,
					   QObject *parent)
	: Tournament(gameManager, engineManager, parent),
	  m_pairNumber(0)
{
}

QString RoundRobinTournament::type() const
{
	return "round-robin";
}

int RoundRobinTournament::gamesPerRound() const
{
	const int count = playerCount() - (playerCount() % 2);

	if (bergerSchedule())
		return count / 2;

	const int totalRounds = finalGameCount() >= count
					? finalGameCount() / count : 1;

	return finalGameCount() / totalRounds;
}

QList< QPair<QString, QString> > RoundRobinTournament::getPairings()
{
	const int count = playerCount() + (playerCount() % 2);
	QList< QPair<QString, QString> > pList;

	if (bergerSchedule())
	{
		const int roundsPerCycle = gamesPerCycle() / (count / 2);
		QList<int> bergerTable;
		QMap<QString, QPair<int, int> > WBmap;
		QString ls;

		initializePairing(bergerTable);

		int bergerPtr = 0;
		int gameNumber = 0;
		int currentRound = 1;

		while(gameNumber < finalGameCount())
		{
			if (bergerPtr >= bergerTable.size())
			{
				for (int i = 0; i < count; i++)
					if (bergerTable[i] != count - 1)
						bergerTable[i] = (bergerTable[i] + (count / 2)) % (count - 1);

				bergerPtr = 0;
				++currentRound;
				bergerTable.insert(!(((currentRound - 1) % roundsPerCycle) % 2),
								   bergerTable.takeAt(bergerTable.indexOf(count - 1)));
			}
			int white = bergerTable[bergerPtr++];
			int black = bergerTable[bergerPtr++];

			if (swapSides() && (gameNumber / gamesPerCycle()) % 2)
				qSwap(white, black);

			if (white < playerCount() && black < playerCount())
			{
				pList.append(qMakePair(playerAt(white).builder()->name(),
									   playerAt(black).builder()->name()));

				++gameNumber;
			}
		}
	}
	else
	{
		QList<int> topHalf;
		QList<int> bottomHalf;
		int pairNumber = 0;
		int gameNumber = 0;

		for (int i = 0; i < count / 2; i++)
			topHalf.append(i);
		for (int i = count - 1; i >= count / 2; i--)
			bottomHalf.append(i);

		while (gameNumber < finalGameCount())
		{
			if (pairNumber >= topHalf.size())
			{
				pairNumber = 0;
				topHalf.insert(1, bottomHalf.takeFirst());
				bottomHalf.append(topHalf.takeLast());
			}

			int white = topHalf.at(pairNumber);
			int black = bottomHalf.at(pairNumber);

			++pairNumber;

			if (white < playerCount() && black < playerCount())
			{
				for(int game = 0; game < gamesPerEncounter(); ++game)
				{
					pList.append(qMakePair(playerAt(white).builder()->name(),
										   playerAt(black).builder()->name()));
					++gameNumber;

					if(swapSides())
						qSwap(white, black);
				}
			}
		}
	}

	return pList;
}

void RoundRobinTournament::initializePairing()
{
	m_pairNumber = 0;
	if (bergerSchedule())
		initializePairing(m_topHalf);
	else
	{
		m_topHalf.clear();
		m_bottomHalf.clear();
		int count = playerCount() + (playerCount() % 2);

		for (int i = 0; i < count / 2; i++)
			m_topHalf.append(i);
		for (int i = count - 1; i >= count / 2; i--)
			m_bottomHalf.append(i);
	}
}

void RoundRobinTournament::initializePairing(QList<int>& bergerTable)
{
	int count = playerCount() + (playerCount() % 2);
	bergerTable.clear();
	bergerTable.reserve(count);
	for (int i  = 0; i < count; ++i)
		bergerTable.append(0);
	for (int i = 0; i < count / 2; ++i)
		bergerTable[i * 2] = i;
	for (int i = count - 1; i >= count / 2; --i)
		bergerTable[((count - i) * 2) - 1] = i;
}

int RoundRobinTournament::gamesPerCycle() const
{
	return (playerCount() * (playerCount() - 1)) / 2;
}

TournamentPair* RoundRobinTournament::nextPair(int gameNumber)
{
	if (gameNumber >= finalGameCount())
		return nullptr;

	int white, black;

	if (bergerSchedule())
	{
		const int count = playerCount() + (playerCount() % 2);
		const int roundsPerCycle = gamesPerCycle() / (count / 2);

		if (m_pairNumber >= m_topHalf.size())
		{
			for (int i = 0; i < count; i++)
				if (m_topHalf[i] != count - 1)
					m_topHalf[i] = (m_topHalf[i] + (count / 2)) % (count - 1);
			m_pairNumber = 0;

			const int cRound = currentRound();
			m_topHalf.insert(!((cRound % roundsPerCycle) % 2),
							   m_topHalf.takeAt(m_topHalf.indexOf(count - 1)));
			setCurrentRound(cRound + 1);
		}

		white = m_topHalf[m_pairNumber++];
		black = m_topHalf[m_pairNumber++];
	}
	else
	{
		if (gameNumber % gamesPerEncounter() != 0)
			return currentPair();

		if (m_pairNumber >= m_topHalf.size())
		{
			m_pairNumber = 0;
			setCurrentRound(currentRound() + 1);
			m_topHalf.insert(1, m_bottomHalf.takeFirst());
			m_bottomHalf.append(m_topHalf.takeLast());
		}

		white = m_topHalf.at(m_pairNumber);
		black = m_bottomHalf.at(m_pairNumber);

		m_pairNumber++;
	}

	// If 'white' or 'black' equals 'playerCount()' it means
	// that it's a "bye" player, that is an empty player that
	// makes the pairings easier to organize. In that case
	// no game is played and we skip to the next pair.
	if (white < playerCount() && black < playerCount())
		return pair(white, black);
	else
		return nextPair(gameNumber);
}
