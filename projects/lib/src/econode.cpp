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

#include "econode.h"
#include <QFile>
#include <QDataStream>
#include <QMutex>
#include "pgnstream.h"

namespace {

int ecoFromString(const QString& ecoString)
{
	if (ecoString.length() < 2)
		return -1;
	int hundreds = ecoString.at(0).toUpper().toLatin1() - 'A';

	bool ok = false;
	int tens = ecoString.right(ecoString.length() - 1).toInt(&ok);
	if (!ok)
		return -1;

	return hundreds * 100 + tens;
}

} // anonymous namespace

QDataStream& operator<<(QDataStream& out, const EcoNode& node)
{
	out << node.m_ecoCode
	    << node.m_opening
	    << node.m_variation;

	return out;
}

QDataStream& operator>>(QDataStream& in, EcoNode& node)
{
	in >> node.m_ecoCode
	   >> node.m_opening
	   >> node.m_variation;

	return in;
}

QStringList EcoNode::s_openings;
QMap<quint64, EcoNode> EcoNode::s_catalog;

void EcoNode::initialize()
{
	static QMutex mutex;
	if (!s_catalog.isEmpty())
		return;

	mutex.lock();
	if (s_catalog.isEmpty())
	{
		Q_INIT_RESOURCE(eco);

		QFile file(":/eco.bin");
		if (!file.open(QIODevice::ReadOnly))
			qWarning("Could not open ECO file");
		else
		{
			QDataStream in(&file);
			in.setVersion(QDataStream::Qt_4_6);
			in >> s_openings >> s_catalog;
		}
	}
	mutex.unlock();
}

void EcoNode::initialize(PgnStream& in)
{
	if (!s_catalog.isEmpty())
		return;

	if (!in.isOpen())
	{
		qWarning("The pgn stream is not open");
		return;
	}

	QMap<QString, int> tmpOpenings;

	PgnGame game;
	while (game.read(in, INT_MAX - 1, false))
	{
		if (!game.moves().isEmpty())
		{
			const QString openingStr = game.tagValue("Opening");
			if (!openingStr.isEmpty())
			{
				int opening = tmpOpenings.value(openingStr, -1);
				if (opening == -1)
				{
					opening = tmpOpenings.count();
					tmpOpenings[openingStr] = opening;
					s_openings.append(openingStr);
				}
				s_catalog[game.key()] = EcoNode(opening,
												game.tagValue("Variation"),
												game.tagValue("ECO"));
			}
		}
	}
}

const EcoNode* EcoNode::find(quint64 key)
{
	if (s_catalog.isEmpty())
		initialize();

	return s_catalog.contains(key) ? &s_catalog[key] : nullptr;
}

void EcoNode::write(const QString& fileName)
{
	if (s_catalog.isEmpty())
		return;

	QFile file(fileName);
	if (!file.open(QIODevice::WriteOnly))
	{
		qWarning("Could not open file %s", qUtf8Printable(fileName));
		return;
	}

	QDataStream out(&file);
	out.setVersion(QDataStream::Qt_4_6);
	out << s_openings << s_catalog;
}

EcoNode::EcoNode()
	: m_ecoCode(-1),
	  m_opening(-1)
{
}

EcoNode::EcoNode(int opening, const QString& variation, const QString& eco)
	: m_ecoCode(ecoFromString(eco)),
	  m_opening(opening),
	  m_variation(variation)
{
}

QString EcoNode::ecoCode() const
{
	if (m_ecoCode < 0)
		return QString();

	QChar segment('A' + m_ecoCode / 100);
	return segment + QString("%1").arg(m_ecoCode % 100, 2, 10, QChar('0'));
}

QString EcoNode::opening() const
{
	return m_opening >= 0 ? s_openings[m_opening] : QString();
}

QString EcoNode::variation() const
{
	return m_variation;
}
