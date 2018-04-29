/*
    This file is part of Cute Chess.

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

#ifndef ECONODE_H
#define ECONODE_H

#include <QStringList>
#include <QMap>
#include "pgngame.h"
class QDataStream;
class PgnStream;

/*!
 * \brief A node in the ECO catalog (Encyclopaedia of Chess Openings)
 *
 * The EcoNode class can be used to generate and query a database (a map) of
 * known chess openings that belong to the Encyclopaedia of Chess Openings.
 * More about ECO: http://en.wikipedia.org/wiki/Encyclopaedia_of_Chess_Openings
 *
 * The ECO catalog can be generated from a PGN collection or from a binary file
 * that's part of the cutechess library (the default). A node corresponding
 * to a PgnGame can be found by the game's moves Zobrist keys to the find()
 * function.
 *
 * \note The Encyclopaedia of Chess Openings only applies to games of standard
 * chess that start from the default starting position.
 */
class LIB_EXPORT EcoNode
{
	public:
		EcoNode();
		/*! Returns the node's ECO code. */
		QString ecoCode() const;
		/*! Returns the node's opening name. */
		QString opening() const;
		/*!
		 * Returns the node's variation name, or an empty string if the node
		 * doesn't have a variation name.
		 */
		QString variation() const;

		/*! Initializes the ECO tree from the internal opening database. */
		static void initialize();
		/*! Initializes the ECO tree by parsing the PGN games in \a in. */
		static void initialize(PgnStream& in);
		/*!
		 * Returns a pointer to the EcoNode with Zobrist key \a key or
		 * nullptr if no EcoNode is present for the key.
		 * initialize() is called first if the tree is uninitialized.
		 */
		static const EcoNode* find(quint64 key);
		/*! Writes the ECO catalog in binary format to \a fileName. */
		static void write(const QString& fileName);

	private:
		friend LIB_EXPORT QDataStream& operator<<(QDataStream& out, const EcoNode& node);
		friend LIB_EXPORT QDataStream& operator>>(QDataStream& in, EcoNode& node);

		EcoNode(int opening, const QString& variation, const QString& eco);

		static QStringList s_openings;
		static QMap<quint64, EcoNode> s_catalog;

		qint16 m_ecoCode;
		qint32 m_opening;
		QString m_variation;
};

/*! Writes the node \a node to stream \a out. */
extern LIB_EXPORT QDataStream& operator<<(QDataStream& out, const EcoNode& node);
/*! Reads a node from stream \a in into \a node. */
extern LIB_EXPORT QDataStream& operator>>(QDataStream& in, EcoNode& node);

#endif // ECONODE_H
