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

#include <csignal>
#include <cstdlib>

#include <QtGlobal>
#include <QDebug>
#include <QTextStream>
#include <QStringList>
#include <QFile>
#include <QMetaType>

#include <mersenne.h>
#include <enginemanager.h>
#include <enginebuilder.h>
#include <gamemanager.h>
#include <tournament.h>
#include <tournamentfactory.h>
#include <board/boardfactory.h>
#include <enginefactory.h>
#include <enginetextoption.h>
#include <openingsuite.h>
#include <sprt.h>
#include <board/syzygytablebase.h>
#include <board/result.h>
#include <jsonparser.h>
#include <jsonserializer.h>
#include <econode.h>
#include <pgnstream.h>

#include "cutechesscoreapp.h"
#include "matchparser.h"
#include "enginematch.h"

namespace {

EngineMatch* s_match = nullptr;

void sigintHandler(int param)
{
	Q_UNUSED(param);
	if (s_match != nullptr)
		s_match->stop();
	else
		abort();
}


struct EngineData
{
	EngineConfiguration config;
	TimeControl tc;
	QString book;
	int bookDepth;

	bool operator==(const EngineData& data) const { return this->config.name() == data.config.name(); }
};

bool readEngineConfig(const QString& name, EngineConfiguration& config)
{
	const auto app = CuteChessCoreApplication::instance();
	const auto engines = app->engineManager()->engines();
	for (const auto& engine : engines)
	{
		if (engine.name() == name)
		{
			config = engine;
			return true;
		}
	}
	return false;
}

OpeningSuite* parseOpenings(const MatchParser::Option& option, Tournament* tournament)
{
	QMap<QString, QString> params =
		option.toMap("file|format=pgn|order=sequential|plies=1024|start=1");
	bool ok = !params.isEmpty();

	OpeningSuite::Format format = OpeningSuite::EpdFormat;
	if (params["format"] == "epd")
		format = OpeningSuite::EpdFormat;
	else if (params["format"] == "pgn")
		format = OpeningSuite::PgnFormat;
	else if (ok)
	{
		qWarning("Invalid opening suite format: \"%s\"",
			 qPrintable(params["format"]));
		ok = false;
	}

	OpeningSuite::Order order = OpeningSuite::SequentialOrder;
	if (params["order"] == "sequential")
		order = OpeningSuite::SequentialOrder;
	else if (params["order"] == "random")
		order = OpeningSuite::RandomOrder;
	else if (ok)
	{
		qWarning("Invalid opening selection order: \"%s\"",
			 qPrintable(params["order"]));
		ok = false;
	}

	int plies = params["plies"].toInt();
	int start = params["start"].toInt();

	ok = ok && plies > 0 && start > 0;
	if (ok)
	{
		tournament->setOpeningDepth(plies);

		OpeningSuite* suite = new OpeningSuite(params["file"],
							   format,
							   order,
							   start - 1);
		if (order == OpeningSuite::RandomOrder)
			qDebug("Indexing opening suite...");
		ok = suite->initialize();
		if (ok)
			return suite;
		delete suite;
	}
	return nullptr;
}

bool parseEngine(const QStringList& args, EngineData& data)
{
	for (const auto& arg : args)
	{
		QString name = arg.section('=', 0, 0);
		QString val = arg.section('=', 1);
		if (name.isEmpty())
			continue;

		if (name == "conf")
		{
			if (!readEngineConfig(val, data.config))
			{
				qWarning() << "Unknown engine configuration:" << val;
				return false;
			}
		}
		else if (name == "name")
			data.config.setName(val);
		else if (name == "cmd")
			data.config.setCommand(val);
		else if (name == "dir")
			data.config.setWorkingDirectory(val);
		else if (name == "arg")
			data.config.addArgument(val);
		else if (name == "proto")
		{
			if (EngineFactory::protocols().contains(val))
				data.config.setProtocol(val);
			else
			{
				qWarning()<< "Usupported chess protocol:" << val;
				return false;
			}
		}
		// Line that are sent to the engine at startup, ie. before
		// starting the chess protocol.
		else if (name == "initstr")
			data.config.addInitString(val.replace("\\n", "\n"));
		// Should the engine be restarted after each game?
		else if (name == "restart")
		{
			EngineConfiguration::RestartMode mode;
			if (val == "auto")
				mode = EngineConfiguration::RestartAuto;
			else if (val == "on")
				mode = EngineConfiguration::RestartOn;
			else if (val == "off")
				mode = EngineConfiguration::RestartOff;
			else
			{
				qWarning() << "Invalid restart mode:" << val;
				return false;
			}

			data.config.setRestartMode(mode);
		}
		// Trust all result claims coming from the engine?
		else if (name == "trust")
		{
			data.config.setClaimsValidated(false);
		}
		// Time control (moves/time+increment)
		else if (name == "tc")
		{
			TimeControl tc(val);
			if (!tc.isValid())
			{
				qWarning() << "Invalid time control:" << val;
				return false;
			}

			data.tc.setInfinity(tc.isInfinite());
			data.tc.setTimePerTc(tc.timePerTc());
			data.tc.setMovesPerTc(tc.movesPerTc());
			data.tc.setTimeIncrement(tc.timeIncrement());
		}
		// Search time per move
		else if (name == "st")
		{
			bool ok = false;
			int moveTime = val.toDouble(&ok) * 1000.0;
			if (!ok || moveTime <= 0)
			{
				qWarning() << "Invalid search time:" << val;
				return false;
			}
			data.tc.setTimePerMove(moveTime);
		}
		// Time expiry margin
		else if (name == "timemargin")
		{
			bool ok = false;
			int margin = val.toInt(&ok);
			if (!ok || margin < 0)
			{
				qWarning() << "Invalid time margin:" << val;
				return false;
			}
			data.tc.setExpiryMargin(margin);
		}
		else if (name == "book")
			data.book = val;
		else if (name == "bookdepth")
		{
			if (val.toInt() <= 0)
			{
				qWarning() << "Invalid book depth limit:" << val;
				return false;
			}
			data.bookDepth = val.toInt();
		}
		else if (name == "whitepov")
		{
			data.config.setWhiteEvalPov(true);
		}
		else if (name == "depth")
		{
			if (val.toInt() <= 0)
			{
				qWarning() << "Invalid depth limit:" << val;
				return false;
			}
			data.tc.setPlyLimit(val.toInt());
		}
		else if (name == "nodes")
		{
			if (val.toInt() <= 0)
			{
				qWarning() << "Invalid node limit:" << val;
				return false;
			}
			data.tc.setNodeLimit(val.toInt());
		}
		else if (name == "ponder")
		{
			data.config.setPondering(true);
		}
		// Custom engine option
		else if (name.startsWith("option."))
			data.config.setOption(name.section('.', 1), val);
		else if (name == "stderr")
			data.config.setStderrFile(val);
		else
		{
			qWarning() << "Invalid engine option:" << name;
			return false;
		}
	}

	return true;
}

EngineMatch* parseMatch(const QStringList& args, QObject* parent)
{
	MatchParser parser(args);
	parser.addOption("-srand", QVariant::UInt, 1, 1);
	parser.addOption("-tournament", QVariant::String, 1, 1);
	parser.addOption("-engine", QVariant::StringList, 1, -1, true);
	parser.addOption("-each", QVariant::StringList, 1);
	parser.addOption("-variant", QVariant::String, 1, 1);
	parser.addOption("-concurrency", QVariant::Int, 1, 1);
	parser.addOption("-draw", QVariant::StringList);
	parser.addOption("-resign", QVariant::StringList);
	parser.addOption("-tb", QVariant::String, 1, 1);
	parser.addOption("-tbpieces", QVariant::Int, 1, 1);
	parser.addOption("-tbignore50", QVariant::Bool, 0, 0);
	parser.addOption("-event", QVariant::String, 1, 1);
	parser.addOption("-games", QVariant::Int, 1, 1);
	parser.addOption("-rounds", QVariant::Int, 1, 1);
	parser.addOption("-sprt", QVariant::StringList);
	parser.addOption("-ratinginterval", QVariant::Int, 1, 1);
	parser.addOption("-debug", QVariant::Bool, 0, 0);
	parser.addOption("-openings", QVariant::StringList);
	parser.addOption("-bookmode", QVariant::String);
	parser.addOption("-pgnout", QVariant::StringList, 1, 2);
	parser.addOption("-epdout", QVariant::String, 1, 1);
	parser.addOption("-repeat", QVariant::Int, 0, 1);
	parser.addOption("-noswap", QVariant::Bool, 0, 0);
	parser.addOption("-recover", QVariant::Bool, 0, 0);
	parser.addOption("-site", QVariant::String, 1, 1);
	parser.addOption("-wait", QVariant::Int, 1, 1);
	parser.addOption("-seeds", QVariant::UInt, 1, 1);
	parser.addOption("-livepgnout", QVariant::StringList, 1, 2);
	parser.addOption("-tournamentfile", QVariant::String, 1, 1);
	parser.addOption("-resume", QVariant::Bool, 0, 0);
	parser.addOption("-ecopgn", QVariant::String, 1, 1);
	parser.addOption("-bergerschedule", QVariant::Bool, 0, 0);
	parser.addOption("-kfactor", QVariant::Double, 1, 1);

	if (!parser.parse())
		return nullptr;

	GameManager* manager = CuteChessCoreApplication::instance()->gameManager();

	QVariantMap tfMap, tMap, eMap;
	QVariantList eList;
	bool wantsResume = false;
	bool wantsDebug = parser.takeOption("-debug").toBool();

	QString ecoPgn = parser.takeOption("-ecopgn").toString();
	if (!ecoPgn.isEmpty())
	{
		if (QFile::exists(ecoPgn))
		{
			QFile input(ecoPgn);
			if (input.open(QIODevice::ReadOnly | QIODevice::Text))
			{
				PgnStream pgnStream(&input);
				EcoNode::initialize(pgnStream);
			}
			else
				qWarning("cannot open eco file %s", qPrintable(ecoPgn));
		}
		else
			qWarning("eco file %s not found", qPrintable(ecoPgn));
	}

	QString tournamentFile = parser.takeOption("-tournamentfile").toString();
	bool usingTournamentFile = false;

	if (!tournamentFile.isEmpty()) {
			if (!tournamentFile.endsWith(".json"))
				tournamentFile.append(".json");
			if (QFile::exists(tournamentFile)) {
				QFile input(tournamentFile);
				if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
					qWarning("cannot open tournament configuration file: %s", qPrintable(tournamentFile));
					return 0;
				}

				QTextStream stream(&input);
				JsonParser jsonParser(stream);
				// we don't want to use the tournament file at all unless wantResume == true
				wantsResume = parser.takeOption("-resume").toBool();
				if (wantsResume) {
					tfMap = jsonParser.parse().toMap();
					if (tfMap.contains("tournamentSettings"))
						tMap = tfMap["tournamentSettings"].toMap();
					if (tfMap.contains("engineSettings"))
						eMap = tfMap["engineSettings"].toMap();
					if (!(tMap.isEmpty() || eMap.isEmpty()))
						usingTournamentFile = true;
				}
			}
	}

	QString ttype;
	if (usingTournamentFile && tMap.contains("type")) // tournament file overrides cli options
		ttype = tMap["type"].toString();
	else
	{
		ttype = parser.takeOption("-tournament").toString();
		if (!ttype.isEmpty())
			tMap.insert("type", ttype);
	}
	if (ttype.isEmpty())
		ttype = "round-robin";
	Tournament* tournament = TournamentFactory::create(ttype, manager, parent);
	if (tournament == nullptr)
	{
		qWarning("Invalid tournament type: %s", qPrintable(ttype));
		return nullptr;
	}

	// seed the generator as necessary -- it is always necessary if we're using a tournament file
	uint srand = 0;
	if (wantsResume) {
		if (tMap.contains("srand")) {
			srand = tMap["srand"].toUInt(); // we want to resume a tournament in progress
		}
		if (!srand) {
			qWarning("Missing random seed; randomly-chosen openings may not be consistent with the previous run.");
		}
	}
	// no srand? check if one is specified in the options
	if (!srand)
		srand = parser.takeOption("-srand").toUInt();
	// still none? if we're using a tournament file, we need one, so let's get one
	if (!srand && !tournamentFile.isEmpty()) {
		QTime time = QTime::currentTime();
		qsrand((uint)time.msec());
		while (!srand) {
			srand = qrand();
		}
	}
	if (srand) {
		Mersenne::initialize(srand);
		tMap.insert("srand", srand);
	}

	EngineMatch* match = new EngineMatch(tournament, parent);
	if (!tournamentFile.isEmpty()) match->setTournamentFile(tournamentFile);

	QList<EngineData> engines;
	QStringList eachOptions;
	GameAdjudicator adjudicator;
	MatchParser::Option openingsOption = {"", QVariant()};
	MatchParser::Option bookmodeOption = {"", QVariant()};

	if (usingTournamentFile) {
		if (tMap.contains("gamesPerEncounter"))
			tournament->setGamesPerEncounter(tMap["gamesPerEncounter"].toInt());
		if (tMap.contains("roundMultiplier"))
			tournament->setRoundMultiplier(tMap["roundMultiplier"].toInt());
		if (tMap.contains("startDelay"))
			tournament->setStartDelay(tMap["startDelay"].toInt());
		if (tMap.contains("name"))
			tournament->setName(tMap["name"].toString());
		if (tMap.contains("site"))
			tournament->setSite(tMap["site"].toString());
		if (tMap.contains("eventDate"))
			tournament->setEventDate(tMap["eventDate"].toString());
		if (tMap.contains("variant"))
			tournament->setVariant(tMap["variant"].toString());
		if (tMap.contains("recoveryMode"))
			tournament->setRecoveryMode(tMap["recoveryMode"].toBool());
		if (tMap.contains("pgnOutput")) {
			if (tMap.contains("pgnOutMode"))
				tournament->setPgnOutput(tMap["pgnOutput"].toString(), (PgnGame::PgnMode)tMap["pgnOutMode"].toInt());
			else
				tournament->setPgnOutput(tMap["pgnOutput"].toString());
		}
		if (tMap.contains("livePgnOutput")) {
			if (tMap.contains("livePgnOutMode"))
				tournament->setLivePgnOutput(tMap["livePgnOutput"].toString(), (PgnGame::PgnMode)tMap["livePgnOutMode"].toInt());
			else
				tournament->setLivePgnOutput(tMap["livePgnOutput"].toString());
		}
		if (tMap.contains("epdOutput"))
			tournament->setEpdOutput(tMap["epdOutput"].toString());
		if (tMap.contains("pgnCleanupEnabled"))
			tournament->setPgnCleanupEnabled(tMap["pgnCleanupEnabled"].toBool());
		if (tMap.contains("openingRepetitions"))
			tournament->setOpeningRepetitions(tMap["openingRepetitions"].toInt());
		if (tMap.contains("concurrency"))
			manager->setConcurrency(tMap["concurrency"].toInt());
		if (tMap.contains("drawAdjudication")) {
			QVariantMap dMap = tMap["drawAdjudication"].toMap();
			if (dMap.contains("movenumber") &&
				dMap.contains("movecount") &&
				dMap.contains("score"))
			{
				adjudicator.setDrawThreshold(
					dMap["movenumber"].toInt(),
					dMap["movecount"].toInt(),
					dMap["score"].toInt());
			}
		}
		if (tMap.contains("resignAdjudication")) {
			QVariantMap rMap = tMap["resignAdjudication"].toMap();
			if (rMap.contains("movecount") &&
				rMap.contains("score"))
			{
				adjudicator.setResignThreshold(rMap["movecount"].toInt(), -(rMap["score"].toInt()));
			}
		}

		if (tMap.contains("swapSides"))
			tournament->setSwapSides(tMap["swapSides"].toBool());

		if (tMap.contains("tb")) {
			adjudicator.setTablebaseAdjudication(true);

			bool ok = SyzygyTablebase::initialize(tMap["tb"].toString()) &&
				 SyzygyTablebase::tbAvailable(3);
			if (!ok)
				qWarning("Could not load Syzygy tablebases");
		}
		if (tMap.contains("tbPieces")) {
			int value = tMap["tbPieces"].toInt();
			if (value > 2)
				SyzygyTablebase::setPieces(value);
		}
		if (tMap.contains("tbIgnore50"))
			if (tMap["tbIgnore50"].toBool())
				SyzygyTablebase::setNoRule50();

		if (tMap.contains("openings")) {
			openingsOption.name = "-openings";
			openingsOption.value = tMap["openings"];
		}
		if (tMap.contains("bookmode")) {
			openingsOption.name = "-bookmode";
			openingsOption.value = tMap["bookmode"];
		}
		if (tMap.contains("bergerSchedule"))
			tournament->setBergerSchedule(tMap["bergerSchedule"].toBool());
		if (eMap.contains("engines")) {
			eList = eMap["engines"].toList();
			for (int e = 0; e < eList.size(); e++) {
				bool ok = true;
				QStringList eData = eList.at(e).toStringList();
				EngineData engine;
				engine.bookDepth = 1000;
				ok = parseEngine(eData, engine);
				if (ok)
					engines.append(engine);
			}
		}
		if (eMap.contains("each")) {
			eachOptions = eMap["each"].toStringList();
		}

		if (tfMap.contains("matchProgress")) {
			if (!wantsResume) {
				tfMap.remove("matchProgress");
			} else {
				QVariantList pList;
				int nextGame = 0;

				pList = tfMap["matchProgress"].toList();
				QVariantList::iterator p;
				for (p = pList.begin(); p != pList.end(); ++p) {
					QVariantMap pMap = p->toMap();
					if (pMap["result"] == "*") {
						pList.erase(p, pList.end());
						break;
					}
				}
				tfMap.insert("matchProgress", pList);
				nextGame = pList.size();
				if (nextGame > 0)
					tournament->setResume(nextGame);
			}
		}
	} else { // !usingTournamentFile
		const auto options = parser.options();
		for (const auto& option : options)
		{
			bool ok = true;
			const QString& name = option.name;
			const QVariant& value = option.value;
			Q_ASSERT(!value.isNull());

			// Chess engine
			if (name == "-engine")
			{
				EngineData engine;
				engine.bookDepth = 1000;
				ok = parseEngine(value.toStringList(), engine);
				if (ok) {
					if (!engines.contains(engine))
						engines.append(engine);
					eList.append(value.toStringList());
				}
			}
			// The engine options that apply to each engine
			else if (name == "-each")
			{
				eachOptions = value.toStringList();
				eMap.insert("each", value.toStringList());
			}
			// Chess variant (default: standard chess)
			else if (name == "-variant")
			{
				ok = Chess::BoardFactory::variants().contains(value.toString());
				if (ok) {
					tournament->setVariant(value.toString());
					tMap.insert("variant", value.toString());
				}
			}
			else if (name == "-concurrency")
			{
				ok = value.toInt() > 0;
				if (ok) {
					manager->setConcurrency(value.toInt());
					tMap.insert("concurrency", value.toInt());
				}
			}
			// Threshold for draw adjudication
			else if (name == "-draw")
			{
				QMap<QString, QString> params =
					option.toMap("movenumber|movecount|score");
				bool numOk = false;
				bool countOk = false;
				bool scoreOk = false;
				int moveNumber = params["movenumber"].toInt(&numOk);
				int moveCount = params["movecount"].toInt(&countOk);
				int score = params["score"].toInt(&scoreOk);

				ok = (numOk && countOk && scoreOk);
				if (ok) {
					adjudicator.setDrawThreshold(moveNumber, moveCount, score);
					QVariantMap dMap;
					dMap.insert("movenumber", moveNumber);
					dMap.insert("movecount", moveCount);
					dMap.insert("score", score);
					tMap.insert("drawAdjudication", dMap);
				}
			}
			// Threshold for resign adjudication
			else if (name == "-resign")
			{
				QMap<QString, QString> params = option.toMap("movecount|score");
				bool countOk = false;
				bool scoreOk = false;
				int moveCount = params["movecount"].toInt(&countOk);
				int score = params["score"].toInt(&scoreOk);

				ok = (countOk && scoreOk);
				if (ok) {
					adjudicator.setResignThreshold(moveCount, -score);
					QVariantMap rMap;
					rMap.insert("movecount", moveCount);
					rMap.insert("score", score);
					tMap.insert("resignAdjudication", rMap);
				}
			}
			// Syzygy tablebase adjudication
			else if (name == "-tb")
			{
				adjudicator.setTablebaseAdjudication(true);
				QString path = value.toString();

				ok = SyzygyTablebase::initialize(path) &&
					 SyzygyTablebase::tbAvailable(3);
				if (ok)
					tMap.insert("tb", path);
				else
					qWarning("Could not load Syzygy tablebases");
			}
			// Syzygy tablebase pieces
			else if (name == "-tbpieces")
			{
				ok = value.toInt() > 2;
				if (ok) {
					SyzygyTablebase::setPieces(value.toInt());
					tMap.insert("tbPieces", value.toInt());
				}
			}
			// Syzygy ignore 50-move-rule
			else if (name == "-tbignore50")
			{
				bool flag = value.toBool();
				if (flag)
					SyzygyTablebase::setNoRule50();
				tMap.insert("tbIgnore50", flag);
			}
			// Event name
			else if (name == "-event")
			{
				tournament->setName(value.toString());
				tMap.insert("name", value.toString());
			}
			// Number of games per encounter
			else if (name == "-games")
			{
				ok = value.toInt() > 0;
				if (ok) {
					tournament->setGamesPerEncounter(value.toInt());
					tMap.insert("gamesPerEncounter", value.toInt());
				}
			}
			// Multiplier for the number of tournament rounds
			else if (name == "-rounds")
			{
				if (!tournament->canSetRoundMultiplier())
				{
					qWarning("Tournament \"%s\" does not support "
						 "user-defined round multipliers",
						 qPrintable(tournament->type()));
					ok = false;
				}
				else
				{
					int rounds = value.toInt(&ok);
					if (rounds <= 0)
						ok = false;
					else {
						tournament->setRoundMultiplier(rounds);
						tMap.insert("roundMultiplier", value.toInt());
					}
				}
			}
			// SPRT-based stopping rule
			else if (name == "-sprt")
			{
				QMap<QString, QString> params = option.toMap("elo0|elo1|alpha|beta");
				bool sprtOk[4];
				double elo0 = params["elo0"].toDouble(sprtOk);
				double elo1 = params["elo1"].toDouble(sprtOk + 1);
				double alpha = params["alpha"].toDouble(sprtOk + 2);
				double beta = params["beta"].toDouble(sprtOk + 3);

				ok = (sprtOk[0] && sprtOk[1] && sprtOk[2] && sprtOk[3]);
				if (ok) {
					tournament->sprt()->initialize(elo0, elo1, alpha, beta);
					QVariantMap sMap;
					sMap.insert("elo0", elo0);
					sMap.insert("elo1", elo1);
					sMap.insert("alpha", alpha);
					sMap.insert("beta", beta);
					tMap.insert("sprt", sMap);
				}
			}
			// Interval for rating list updates
			else if (name == "-ratinginterval")
			{
				match->setRatingInterval(value.toInt());
				tMap.insert("ratingInterval", value.toInt());
			}
			// Use an opening suite
			else if (name == "-openings")
				openingsOption = option;
			else if (name == "-bookmode")
				bookmodeOption = option;
			// PGN file where the games should be saved
			else if (name == "-pgnout")
			{
				PgnGame::PgnMode mode = PgnGame::Verbose;
				QStringList list = value.toStringList();
				if (list.size() == 2)
				{
					if (list.at(1) == "min")
						mode = PgnGame::Minimal;
					else
						ok = false;
				}
				if (ok) {
					tournament->setPgnOutput(list.at(0), mode);
					tMap.insert("pgnOutput", list.at(0));
					tMap.insert("pgnOutMode", mode);
				}
			}
			// TCEC live PGN file
			else if (name == "-livepgnout")
			{
				PgnGame::PgnMode mode = PgnGame::Verbose;
				QStringList list = value.toStringList();
				if (list.size() == 2)
				{
					if (list.at(1) == "min")
						mode = PgnGame::Minimal;
					else
						ok = false;
				}
				if (ok) {
					tournament->setLivePgnOutput(list.at(0), mode);
					tMap.insert("livePgnOutput", list.at(0));
					tMap.insert("livePgnOutMode", mode);
				}
			}
			// FEN/EPD output file to save positions
			else if (name == "-epdout")
			{
				QString fileName = value.toString();
				tournament->setEpdOutput(fileName);
				tMap.insert("epdOutput", fileName);
			}
			// Play every opening twice (default), or multiple times
			else if (name == "-repeat")
			{
				int rep = value.toInt(&ok);

				if (option.value.type() == QVariant::Bool)
					rep = 2; // default
				if (ok && rep >= 1)
				{
					tournament->setOpeningRepetitions(rep);
					tMap.insert("openingRepetitions", rep);

					if (tournament->gamesPerEncounter() % rep)
						qWarning("%d opening repetitions vs"
							" %d games per encounter",
							rep,
							tournament->gamesPerEncounter());
				}
				else
					ok = false;
			}
			// Do not swap sides between paired engines
			else if (name == "-noswap")
			{
				tournament->setSwapSides(false);
				tMap.insert("swapSides", false);
			}
			// Recover crashed/stalled engines
			else if (name == "-recover")
			{
				tournament->setRecoveryMode(true);
				tMap.insert("recoveryMode", true);
			}
			// Site/location name
			else if (name == "-site")
			{
				tournament->setSite(value.toString());
				tMap.insert("site", value.toString());
			}
			// Delay between games
			else if (name == "-wait")
			{
				ok = value.toInt() >= 0;
				if (ok) {
					tournament->setStartDelay(value.toInt());
					tMap.insert("startDelay", value.toInt());
				}
			}
			// How many players should be seeded?
			else if (name == "-seeds")
			{
				uint seedCount = value.toUInt(&ok);
				if (ok) {
					tournament->setSeedCount(seedCount);
					tMap.insert("seeds", seedCount);
				}
			}
			// Resume a TCEC tournament
			else if (name == "-resume") {
				if (!tournamentFile.isEmpty())
					qWarning("Cannot resume a non-initialized tournament. Creating new tournament file @ %s", qPrintable(tournamentFile));
				else
					qWarning("The -resume flag is meant to be used with the -tournamentfile option. Ignoring.");
			}
			else if(name == "-bergerschedule") {
				bool flag = value.toBool();
				tournament->setBergerSchedule(flag);
				tMap.insert("bergerSchedule", flag);
			}
			else if (name == "-kfactor") {
				const qreal val = value.toDouble();
				ok = val >= 1.0 && val <= 200.0;
				if (ok)
					tMap.insert("eloKfactor", val);
				else
					qWarning("Invalid K-factor %f", val);
			}
			else
				qFatal("Unknown argument: \"%s\"", qPrintable(name));

			if (!ok)
			{
				// Empty values default to boolean type
				if (value.isValid() && value.type() == QVariant::Bool)
					qWarning("Empty value for option \"%s\"",
						 qPrintable(name));
				else
				{
					QString val;
					if (value.type() == QVariant::StringList)
						val = value.toStringList().join(" ");
					else
						val = value.toString();
					qWarning("Invalid value for option \"%s\": \"%s\"",
						 qPrintable(name), qPrintable(val));
				}

				delete match;
				delete tournament;
				return nullptr;
			}
		}
	}

	bool ok = true;

	// Debugging mode. Prints all engine input and output.
	if (wantsDebug)
		match->setDebugMode(true);

	if (tMap.contains("eloKfactor"))
		match->setEloKfactor(tMap["eloKfactor"].toDouble());

	if (!eachOptions.isEmpty())
	{
		QList<EngineData>::iterator it;
		for (it = engines.begin(); it != engines.end(); ++it)
		{
			ok = parseEngine(eachOptions, *it);
			if (!ok)
				break;
		}
	}

	const auto& constEngines = engines;
	for (const auto& engine : constEngines)
	{
		if (!engine.tc.isValid())
		{
			ok = false;
			qWarning("Invalid or missing time control");
			break;
		}

		if (engine.config.command().isEmpty())
		{
			ok = false;
			qCritical("missing chess engine command");
			break;
		}

		if (engine.config.protocol().isEmpty())
		{
			ok = false;
			qWarning("Missing chess protocol");
			break;
		}

		tournament->addPlayer(new EngineBuilder(engine.config),
				      engine.tc,
				      match->addOpeningBook(engine.book),
				      engine.bookDepth);
	}

	if (!openingsOption.name.isEmpty()) {
		OpeningSuite* suite = parseOpenings(openingsOption, tournament);
		if (suite) {
			tournament->setOpeningSuite(suite);
			tMap.insert("openings", openingsOption.value);
		}
		else
			ok = false;
	}

	if (!bookmodeOption.name.isEmpty()) {
		QString val = bookmodeOption.value.toString();
		if (val == "ram")
			match->setBookMode(OpeningBook::Ram);
		else if (val == "disk")
			match->setBookMode(OpeningBook::Disk);
		else
			ok = false;
	}

	if (engines.size() < 2)
	{
		qWarning("At least two engines are needed");
		ok = false;
	}

	if (!ok)
	{
		delete match;
		delete tournament;
		return nullptr;
	}

	if (!tournamentFile.isEmpty() && !tMap.isEmpty()) {
		QFile output(tournamentFile);
		if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
			qWarning("cannot open tournament configuration file: %s", qPrintable(tournamentFile));
			return 0;
		}

		if (!wantsResume || !tMap.contains("eventDate")) {
			QString eventDate = QDate::currentDate().toString("yyyy.MM.dd");
			tournament->setEventDate(eventDate);
			tMap.insert("eventDate", eventDate);
		}

		tfMap.insert("tournamentSettings", tMap);
		eMap.insert("engines", eList);
		tfMap.insert("engineSettings", eMap);

		QTextStream out(&output);
		JsonSerializer serializer(tfMap);
		serializer.serialize(out);
	}

	tournament->setAdjudicator(adjudicator);

	return match;
}

} // anonymous namespace

int main(int argc, char* argv[])
{
	// Register types for signal / slot connections
	qRegisterMetaType<Chess::Result>("Chess::Result");

	setvbuf(stdout, nullptr, _IONBF, 0);
	signal(SIGINT, sigintHandler);

	CuteChessCoreApplication app(argc, argv);

	QStringList arguments = CuteChessCoreApplication::arguments();
	arguments.takeFirst(); // application name

	// Use trivial command-line parsing for now
	QTextStream out(stdout);
	const auto& constArguments = arguments;
	for (const auto& arg : constArguments)
	{
		if (arg == "-v" || arg == "--version" || arg == "-version")
		{
			out << "cutechess-cli " << CUTECHESS_CLI_VERSION << endl;
			out << "Using Qt version " << qVersion() << endl << endl;
			out << "Copyright (C) 2008-2017 Ilari Pihlajisto and Arto Jonsson" << endl;
			out << "\t      2014 Jeremy Bernstein" << endl;
			out << "\t      2018 Guy Vreuls" << endl;
			out << "This is free software; see the source for copying ";
			out << "conditions.  There is NO" << endl << "warranty; not even for ";
			out << "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.";
			out << endl << endl;

			return 0;
		}
		else if (arg == "--engines" || arg == "-engines")
		{
			const auto engines = app.engineManager()->engines();
			for (const auto& engine : engines)
				out << engine.name() << endl;

			return 0;
		}
		else if (arg == "--help" || arg == "-help")
		{
			QFile file(":/help.txt");
			if (file.open(QIODevice::ReadOnly | QIODevice::Text))
				out << file.readAll();
			return 0;
		}
	}

	s_match = parseMatch(arguments, &app);
	if (s_match == nullptr)
		return 1;
	QObject::connect(s_match, SIGNAL(finished()), &app, SLOT(quit()));

	s_match->start();
	return app.exec();
}
