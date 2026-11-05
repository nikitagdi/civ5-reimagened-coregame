// rt_autochoice, part 1 (task F3): technology, free technology, policy (choose only), ideology; part 2 (task F4): pantheon, religion,
// enhancement, reformation, promotions, great people (free, faith, Maya), captured city. See rt_autochoice.h for the rules.
// part 3 (task F5): World Congress proposals and votes, archaeology, ruin rewards, tech steal, STUCK reasons.
// SIMPLIFIED(SIMP-002): the AI picks research, policies, the ideology, beliefs, promotions, great people and the fate of captured cities
// for the human; original: the player picks in TechPopup / SocialPolicyPopup / ChooseIdeologyPopup / the pantheon, religion and
// reformation popups / the promotion buttons / the great person popups / CityCapturedPopup.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "CvCultureClasses.h"
#include "CvDiplomacyAI.h"
#include "CvEspionageClasses.h"
#include "CvGameCoreUtils.h"
#include "CvGrandStrategyAI.h"
#include "CvPolicyAI.h"
#include "CvPolicyClasses.h"
#include "CvReligionClasses.h"
#include "CvTechAI.h"
#include "CvTechClasses.h"
#include "CvTraitClasses.h"
#include "CvTypes.h"
#include "CvVotingClasses.h"
#include "rt_active.h"
#include "rt_autochoice.h"
#include "rt_session.h"
#include "rt_ui.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

CvPolicyAI* PolicyAIOf(CvPlayerPolicies* policies) { return policies ? policies->GetPolicyAI() : NULL; }

std::string Localized(const char* key) { return (key && *key) ? std::string(GetLocalizedText(key).c_str()) : std::string(); }

const CvBeliefEntry* BeliefInfo(BeliefTypes b)
{
	CvBeliefXMLEntries* all = GC.GetGameBeliefs();
	return (all && b != NO_BELIEF) ? all->GetEntry((int)b) : NULL;
}

std::string BeliefName(BeliefTypes b)
{
	const CvBeliefEntry* info = BeliefInfo(b);
	if (!info) return "?";
	const std::string name = Localized(info->getShortDescription());  // the long description is the whole effect text
	return name.empty() ? std::string(info->GetType()) : name;
}

// The gamecore AI function's belief when it is one of the available ones; else the first available one (documented fallback: the AI function
// found no belief scoring above 0). byAI is cleared when the fallback was taken. NO_BELIEF when nothing is available.
BeliefTypes PickBelief(BeliefTypes chosen, const std::vector<BeliefTypes>& available, bool& byAI)
{
	for (size_t i = 0; i < available.size(); ++i)
		if (available[i] == chosen && chosen != NO_BELIEF) return chosen;
	byAI = false;
	return available.empty() ? NO_BELIEF : available[0];
}

// A Great Prophet is useful only to a player who can still found a religion or has one that is not enhanced yet (the test CvPlayerTraits::ChooseMayaBoost makes).
bool ProphetUseful(CvPlayer& p)
{
	CvGameReligions* religions = GC.getGame().GetGameReligions();
	if (!religions) return true;
	const ReligionTypes created = religions->GetReligionCreatedByPlayer(p.GetID());
	if (created == NO_RELIGION) return religions->GetNumReligionsStillToFound() > 0;
	const CvReligion* r = religions->GetReligion(created, p.GetID());
	return !r || !r->m_bEnhanced;
}

UnitTypes UnitByType(const char* type) { return (UnitTypes)GC.getInfoTypeForString(type, true); }

std::string UnitName(UnitTypes u)
{
	const CvUnitEntry* info = GC.getUnitInfo(u);
	if (!info) return "?";
	const std::string name = Localized(info->GetDescriptionKey());
	return name.empty() ? std::string(info->GetType()) : name;
}

// CvPlayerAI::AI_chooseFreeGreatPerson's rule (CvPlayerAI.cpp:452-480), as a function. NO_UNIT where the AI chooses none.
UnitTypes StrategicGreatPerson(CvPlayer& p)
{
	CvDiplomacyAI* diplo = p.GetDiplomacyAI();
	if (diplo && diplo->GetWonderCompetitiveness() >= 8 && GC.getGame().getGameTurn() <= (GC.getGame().getEstimateEndTurn() / 2)) return UnitByType("UNIT_ENGINEER");
	CvGrandStrategyAI* strategy = p.GetGrandStrategyAI();
	if (!strategy) return NO_UNIT;
	const AIGrandStrategyTypes active = strategy->GetActiveGrandStrategy();
	if (active == (AIGrandStrategyTypes)GC.getInfoTypeForString("AIGRANDSTRATEGY_CONQUEST", true)) return UnitByType("UNIT_GREAT_GENERAL");
	if (active == (AIGrandStrategyTypes)GC.getInfoTypeForString("AIGRANDSTRATEGY_CULTURE", true)) return UnitByType("UNIT_ARTIST");
	if (active == (AIGrandStrategyTypes)GC.getInfoTypeForString("AIGRANDSTRATEGY_UNITED_NATIONS", true)) return UnitByType("UNIT_MERCHANT");
	if (active == (AIGrandStrategyTypes)GC.getInfoTypeForString("AIGRANDSTRATEGY_SPACESHIP", true)) return UnitByType("UNIT_SCIENTIST");
	return NO_UNIT;
}

// The auto_choices flag of a blocker: -1 when none, else its value.
int FlagValue(const RtAutoChoices& f, int blocking)
{
	switch (blocking)
	{
	case ENDTURN_BLOCKING_RESEARCH:
	case ENDTURN_BLOCKING_FREE_TECH: return f.research;
	case ENDTURN_BLOCKING_POLICY:
	case ENDTURN_BLOCKING_FREE_POLICY: return f.policy;
	case ENDTURN_BLOCKING_CHOOSE_IDEOLOGY: return f.ideology;
	case ENDTURN_BLOCKING_FOUND_PANTHEON:
	case ENDTURN_BLOCKING_FOUND_RELIGION:
	case ENDTURN_BLOCKING_ENHANCE_RELIGION:
	case ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF: return f.religion;
	case ENDTURN_BLOCKING_UNIT_PROMOTION: return f.promotion;
	case ENDTURN_BLOCKING_FREE_ITEMS:
	case ENDTURN_BLOCKING_FAITH_GREAT_PERSON:
	case ENDTURN_BLOCKING_MAYA_LONG_COUNT: return f.great_person;
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS:
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES: return f.league;
	case ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY: return f.archaeology;
	case ENDTURN_BLOCKING_STEAL_TECH: return f.steal_tech;
	default: return -1;
	}
}

// The gamecore's text markup ([COLOR_...], [ENDCOLOR], [NEWLINE], [ICON_...]) is dropped from a journal line.
std::string PlainText(const std::string& in)
{
	std::string out;
	for (size_t i = 0; i < in.size(); ++i)
	{
		if (in[i] == '[')
		{
			const size_t close = in.find(']', i);
			bool tag = close != std::string::npos && close > i + 1;
			for (size_t j = i + 1; tag && j < close; ++j) tag = (in[j] >= 'A' && in[j] <= 'Z') || in[j] == '_' || (in[j] >= '0' && in[j] <= '9');
			if (tag) { i = close; continue; }
		}
		out += in[i];
	}
	return out;
}

CvLeague* ActiveLeague()
{
	CvGameLeagues* leagues = GC.getGame().GetGameLeagues();
	return leagues ? leagues->GetActiveLeague() : NULL;
}

std::string GoodyName(GoodyTypes g, std::string& typeOut)
{
	Database::SingleResult result;
	CvGoodyInfo info;
	if (DB.SelectAt(result, "GoodyHuts", (int)g)) info.CacheResult(result);
	typeOut = info.GetType() ? info.GetType() : "?";
	std::string name = PlainText(Localized(info.GetDescription()));
	for (size_t i; (i = name.find("  ")) != std::string::npos;) name.erase(i, 1);  // the text has double spaces where an icon was
	while (!name.empty() && name[0] == ' ') name.erase(0, 1);
	return name.empty() ? typeOut : name;
}

}  // namespace

const char* RtAutoChoice::FlagName(int blocking)
{
	switch (blocking)
	{
	case ENDTURN_BLOCKING_RESEARCH:
	case ENDTURN_BLOCKING_FREE_TECH: return "research";
	case ENDTURN_BLOCKING_POLICY:
	case ENDTURN_BLOCKING_FREE_POLICY: return "policy";
	case ENDTURN_BLOCKING_CHOOSE_IDEOLOGY: return "ideology";
	case ENDTURN_BLOCKING_FOUND_PANTHEON:
	case ENDTURN_BLOCKING_FOUND_RELIGION:
	case ENDTURN_BLOCKING_ENHANCE_RELIGION:
	case ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF: return "religion";
	case ENDTURN_BLOCKING_UNIT_PROMOTION: return "promotion";
	case ENDTURN_BLOCKING_FREE_ITEMS:
	case ENDTURN_BLOCKING_FAITH_GREAT_PERSON:
	case ENDTURN_BLOCKING_MAYA_LONG_COUNT: return "great_person";
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS:
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES: return "league";
	case ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY: return "archaeology";
	case ENDTURN_BLOCKING_STEAL_TECH: return "steal_tech";
	default: return NULL;
	}
}

std::string RtAutoChoice::StuckReason(RtSession& s, int blocking)
{
	const char* flag = FlagName(blocking);
	if (!flag) return std::string();
	const int value = FlagValue(Flags(s), blocking);
	const int turn = GC.getGame().getGameTurn();
	const PlayerTypes who = s.Engine().GetActivePlayer();
	const Counters* cnt = (who != NO_PLAYER && (int)who < MAX_PLAYERS && m_by[(int)who].turn == turn) ? &m_by[(int)who] : NULL;
	if (!value)
		return Format("the human is blocked by %s and nobody can answer it (no human input); auto_choices.%s is off, set it to 1 for the AI to choose", RtSession::BlockingName(blocking), flag);
	return Format("the human is blocked by %s and nobody can answer it (no human input); auto_choices.%s is on but the automatic choice did not clear it (%d attempt(s) in turn %d%s)",
		RtSession::BlockingName(blocking), flag, (cnt && blocking >= 0 && blocking < 64) ? cnt->attempts[blocking] : 0, turn,
		(cnt && blocking >= 0 && blocking < 64 && cnt->gaveUpLogged[blocking]) ? ", limit reached" : "");
}

// Ruin rewards waiting for `who` (NO_PLAYER: for everybody). The list is the session's, with the rewards of every human in a network game.
int RtAutoChoice::PendingGoodies(RtSession& s, PlayerTypes who)
{
	const std::vector<RtUserInterface::PendingGoody>& all = s.UI().PendingGoodies();
	int n = 0;
	for (size_t i = 0; i < all.size(); ++i)
		if (who == NO_PLAYER || all[i].player == (int)who) ++n;
	return n;
}

RtAutoChoice::RtAutoChoice() : m_flagsSet(false), m_choices(0)
{
	memset(m_by, 0, sizeof(m_by));
	for (int i = 0; i < MAX_PLAYERS; ++i) m_by[i].turn = -1;
}

const char* RtAutoChoice::KindName(int kind)
{
	static const char* const names[] = {"RESEARCH", "FREE_TECH", "POLICY", "POLICY_BRANCH", "IDEOLOGY", "PANTHEON", "RELIGION", "ENHANCEMENT", "REFORMATION",
		"PROMOTION", "FREE_GREAT_PERSON", "FAITH_GREAT_PERSON", "MAYA_GREAT_PERSON", "CAPTURED_CITY", "LEAGUE_PROPOSAL", "LEAGUE_VOTE", "ARCHAEOLOGY", "GOODY_HUT",
		"STEAL_TECH"};
	return (kind >= 0 && kind <= RtAutoChoiceEvent::STEAL_TECH) ? names[kind] : "?";
}

const RtAutoChoices& RtAutoChoice::Flags(const RtSession& session)
{
	if (!m_flagsSet)
	{
		m_flags = session.Setup().auto_choices;
		m_flagsSet = true;
	}
	return m_flags;
}

void RtAutoChoice::SetFlags(const RtAutoChoices& flags)
{
	m_flags = flags;
	m_flagsSet = true;
}

bool RtAutoChoice::PopEvent(RtAutoChoiceEvent& out)
{
	if (m_events.empty()) return false;
	out = m_events.front();
	m_events.pop_front();
	return true;
}

bool RtAutoChoice::Run(RtSession& s, PlayerTypes who)
{
	ActiveAs as(who);
	return Run(s);
}

bool RtAutoChoice::Run(RtSession& s)
{
	if (!s.InGame()) return false;
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	if (me == NO_PLAYER || (int)me >= MAX_PLAYERS) return false;
	CvPlayer& p = GET_PLAYER(me);
	// The human's own turn, not ended, and the previous command (ours or the human's) delivered.
	if (!p.isHuman() || !p.isAlive() || !p.isTurnActive() || e.HasSentTurnComplete() || e.NetQueueSize() > 0) return false;
	const int blocking = (int)p.GetEndTurnBlockingType();
	const RtAutoChoices& f = Flags(s);

	// The attempt counters are this human's (a shared counter ran out for the fifth human of a turn).
	Counters& cnt = m_by[(int)me];
	int* const attempts = cnt.attempts;
	bool* const gaveUpLogged = cnt.gaveUpLogged;
	const int turn = GC.getGame().getGameTurn();
	if (turn != cnt.turn)
	{
		cnt.turn = turn;
		memset(cnt.attempts, 0, sizeof(cnt.attempts));
		memset(cnt.gaveUpLogged, 0, sizeof(cnt.gaveUpLogged));
	}

	// A ruin reward is no blocker either (a popup): the pending goody is the state.
	if (f.goody_hut && PendingGoodies(s, me) > 0)
	{
		if (attempts[kGoodySlot] < kMaxGoodyAttempts)
		{
			if (ChooseGoody(s))
			{
				++attempts[kGoodySlot];
				return true;
			}
		}
		else if (!gaveUpLogged[kGoodySlot])
		{
			gaveUpLogged[kGoodySlot] = true;
			Log(LOG_WARN, "autochoice: turn %d: a ruin reward is still undecided after %d automatic choice(s); left to the human", turn, kMaxGoodyAttempts);
		}
	}

	// A captured city is no blocker (the popup is not a notification): its flag is the pending state.
	if (f.captured_city)
	{
		if (attempts[kCapturedSlot] < kMaxCapturedCityAttempts)
		{
			if (ChooseCapturedCity(s))
			{
				++attempts[kCapturedSlot];
				return true;
			}
		}
		else if (!gaveUpLogged[kCapturedSlot])
		{
			gaveUpLogged[kCapturedSlot] = true;
			Log(LOG_WARN, "autochoice: turn %d: a captured city is still undecided after %d automatic choice(s); left to the human", turn, kMaxCapturedCityAttempts);
		}
	}

	if (blocking < 0 || blocking >= 64) return false;
	const bool on = FlagValue(f, blocking) > 0;
	if (!on) return false;  // not an automatic blocker, or its flag is off: the blocker stays for the human

	const int maxAttempts = blocking == ENDTURN_BLOCKING_UNIT_PROMOTION ? kMaxPromotionAttempts : blocking == ENDTURN_BLOCKING_STEAL_TECH ? kMaxStealAttempts : kMaxAttempts;
	if (attempts[blocking] >= maxAttempts)
	{
		if (!gaveUpLogged[blocking])
		{
			gaveUpLogged[blocking] = true;
			Log(LOG_WARN, "autochoice: turn %d: %s is still there after %d automatic choice(s); left to the human", turn, RtSession::BlockingName(blocking),
				maxAttempts);
		}
		return false;
	}

	bool chose = false;
	switch (blocking)
	{
	case ENDTURN_BLOCKING_RESEARCH: chose = ChooseTech(s, blocking, false); break;
	case ENDTURN_BLOCKING_FREE_TECH: chose = ChooseTech(s, blocking, true); break;
	case ENDTURN_BLOCKING_POLICY:
	case ENDTURN_BLOCKING_FREE_POLICY: chose = ChoosePolicy(s, blocking); break;
	case ENDTURN_BLOCKING_CHOOSE_IDEOLOGY: chose = ChooseIdeology(s, blocking); break;
	case ENDTURN_BLOCKING_FOUND_PANTHEON: chose = ChoosePantheon(s, blocking); break;
	case ENDTURN_BLOCKING_FOUND_RELIGION: chose = ChooseReligion(s, blocking); break;
	case ENDTURN_BLOCKING_ENHANCE_RELIGION: chose = ChooseEnhancement(s, blocking); break;
	case ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF: chose = ChooseReformation(s, blocking); break;
	case ENDTURN_BLOCKING_UNIT_PROMOTION: chose = ChoosePromotion(s, blocking); break;
	case ENDTURN_BLOCKING_FREE_ITEMS:
	case ENDTURN_BLOCKING_FAITH_GREAT_PERSON: chose = ChooseGreatPerson(s, blocking); break;
	case ENDTURN_BLOCKING_MAYA_LONG_COUNT: chose = ChooseMaya(s, blocking); break;
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS: chose = ChooseLeagueProposals(s, blocking); break;
	case ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES: chose = ChooseLeagueVotes(s, blocking); break;
	case ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY: chose = ChooseArchaeology(s, blocking); break;
	case ENDTURN_BLOCKING_STEAL_TECH: chose = ChooseStealTech(s, blocking); break;
	default: break;
	}
	if (chose) ++attempts[blocking];
	return chose;
}

void RtAutoChoice::Record(RtAutoChoiceEvent::Kind kind, int blocking, int id, bool ai, const char* type, const char* descKey, const char* textFmt, const char* rule)
{
	RtAutoChoiceEvent ev;
	ev.kind = kind;
	ev.turn = GC.getGame().getGameTurn();
	ev.player = (int)GC.getGame().getActivePlayer();
	ev.blocking = blocking;
	ev.id = id;
	ev.ai = ai;
	ev.type = type ? type : "";
	ev.name = Localized(descKey);
	if (ev.name.empty()) ev.name = ev.type;
	ev.text = Format(textFmt, ev.name.c_str());
	ev.rule = rule;
	m_events.push_back(ev);
	++m_choices;
	Log(LOG_INFO, "autochoice: turn %d: %s (%s, %s, blocker %s)", ev.turn, ev.text.c_str(), ev.type.c_str(), ev.rule.c_str(), RtSession::BlockingName(blocking));
}

void RtAutoChoice::RecordText(RtAutoChoiceEvent::Kind kind, int blocking, int id, bool ai, const char* type, const std::string& name, const std::string& text, const char* rule)
{
	RtAutoChoiceEvent ev;
	ev.kind = kind;
	ev.turn = GC.getGame().getGameTurn();
	ev.player = (int)GC.getGame().getActivePlayer();
	ev.blocking = blocking;
	ev.id = id;
	ev.ai = ai;
	ev.type = type ? type : "";
	ev.name = name;
	ev.text = text;
	ev.rule = rule;
	m_events.push_back(ev);
	++m_choices;
	Log(LOG_INFO, "autochoice: turn %d: %s (%s, %s, blocker %s)", ev.turn, ev.text.c_str(), ev.type.c_str(), ev.rule.c_str(), RtSession::BlockingName(blocking));
}

bool RtAutoChoice::ChooseTech(RtSession& s, int blocking, bool free)
{
	CvPlayer& p = GET_PLAYER(s.Engine().GetActivePlayer());
	CvPlayerTechs* techs = p.GetPlayerTechs();
	if (!techs) return false;
	if (free ? p.GetNumFreeTechs() <= 0 : techs->GetCurrentResearch() != NO_TECH) return false;  // already chosen
	// SIMPLIFIED(SIMP-002): the AI picks research for the human; original: the player picks in TechPopup.
	CvTechAI* ai = techs->GetTechAI();
	TechTypes tech = ai ? ai->ChooseNextTech(&p, free) : NO_TECH;
	bool byAI = true;
	if (tech == NO_TECH || !(free ? techs->CanResearchForFree(tech) : techs->CanResearch(tech)))
	{
		// Fallback (ChooseNextTech found no weighted choice): the cheapest valid tech, lowest id on a tie.
		byAI = false;
		tech = NO_TECH;
		int best = 0;
		for (int i = 0; i < GC.getNumTechInfos(); ++i)
		{
			const TechTypes t = (TechTypes)i;
			if (!(free ? techs->CanResearchForFree(t) : techs->CanResearch(t))) continue;
			const int cost = techs->GetResearchCost(t);
			if (tech == NO_TECH || cost < best) { tech = t; best = cost; }
		}
	}
	if (tech == NO_TECH) return false;
	// As TechPopup / TechTree send it: iDiscover = the free techs left for a free choice, 0 for research.
	s.Engine().sendResearch(tech, free ? p.GetNumFreeTechs() : 0, NO_PLAYER, false);
	const CvTechEntry* info = GC.getTechInfo(tech);
	Record(free ? RtAutoChoiceEvent::FREE_TECH : RtAutoChoiceEvent::RESEARCH, blocking, (int)tech, byAI, info ? info->GetType() : "?", info ? info->GetDescriptionKey() : NULL,
		free ? "Free technology: %s (chosen automatically)" : "Researching %s (chosen automatically)", byAI ? "CvTechAI::ChooseNextTech" : "cheapest");
	return true;
}

bool RtAutoChoice::ChoosePolicy(RtSession& s, int blocking)
{
	CvPlayer& p = GET_PLAYER(s.Engine().GetActivePlayer());
	CvPlayerPolicies* policies = p.GetPlayerPolicies();
	CvPolicyAI* ai = PolicyAIOf(policies);
	if (!ai) return false;
	// CvPlayerPolicies::DoPolicyAI's conditions for one more adoption.
	if (p.getNextPolicyCost() <= 0) return false;
	if (p.getJONSCulture() < p.getNextPolicyCost() && p.GetNumFreePolicies() <= 0 && p.GetNumFreeTenets() <= 0) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the policy for the human (DoPolicyAI without DoConsiderIdeologySwitch: choose only, never switch
	// the ideology); original: the player picks in SocialPolicyPopup.
	const int next = (int)ai->ChooseNextPolicy(&p);
	if (next == (int)NO_POLICY || next < 0) return false;
	const int branches = GC.getNumPolicyBranchInfos();
	if (next < branches)
	{
		const PolicyBranchTypes branch = (PolicyBranchTypes)next;
		s.Engine().sendUpdatePolicies(false, (int)branch, true);
		const CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(branch);
		Record(RtAutoChoiceEvent::POLICY_BRANCH, blocking, (int)branch, true, info ? info->GetType() : "?", info ? info->GetDescriptionKey() : NULL,
			"Adopted the policy branch %s (chosen automatically)", "CvPolicyAI::ChooseNextPolicy");
	}
	else
	{
		const PolicyTypes policy = (PolicyTypes)(next - branches);
		s.Engine().sendUpdatePolicies(true, (int)policy, true);
		const CvPolicyEntry* info = GC.getPolicyInfo(policy);
		Record(RtAutoChoiceEvent::POLICY, blocking, (int)policy, true, info ? info->GetType() : "?", info ? info->GetDescriptionKey() : NULL,
			"Adopted the policy %s (chosen automatically)", "CvPolicyAI::ChooseNextPolicy");
	}
	return true;
}

bool RtAutoChoice::ChooseIdeology(RtSession& s, int blocking)
{
	CvPlayer& p = GET_PLAYER(s.Engine().GetActivePlayer());
	CvPlayerPolicies* policies = p.GetPlayerPolicies();
	if (!policies || !policies->IsTimeToChooseIdeology() || policies->GetLateGamePolicyTree() != NO_POLICY_BRANCH_TYPE) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the ideology for the human; original: the player picks in ChooseIdeologyPopup.
	// DoChooseIdeology applies its choice itself (SetPolicyBranchUnlocked(branch, true, false) = ResponseIdeologyChoice); see the header.
	policies->DoChooseIdeology();
	const PolicyBranchTypes branch = policies->GetLateGamePolicyTree();
	if (branch == NO_POLICY_BRANCH_TYPE)
	{
		Log(LOG_WARN, "autochoice: turn %d: CvPolicyAI::DoChooseIdeology chose no ideology", GC.getGame().getGameTurn());
		return false;
	}
	const CvPolicyBranchEntry* info = GC.getPolicyBranchInfo(branch);
	Record(RtAutoChoiceEvent::IDEOLOGY, blocking, (int)branch, true, info ? info->GetType() : "?", info ? info->GetDescriptionKey() : NULL,
		"Adopted the ideology %s (chosen automatically)", "CvPolicyAI::DoChooseIdeology");
	return true;
}

// ---- part 2 (F4) ----

bool RtAutoChoice::ChoosePantheon(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvGameReligions* religions = GC.getGame().GetGameReligions();
	CvReligionAI* ai = p.GetReligionAI();
	if (!religions || !ai || religions->CanCreatePantheon(me, true) != CvGameReligions::FOUNDING_OK) return false;  // the state does not ask for it (any more)
	// SIMPLIFIED(SIMP-002): the AI picks the pantheon belief for the human; original: the player picks in the pantheon popup.
	bool byAI = true;
	const BeliefTypes belief = PickBelief(ai->ChoosePantheonBelief(), religions->GetAvailablePantheonBeliefs(), byAI);
	if (belief == NO_BELIEF) return false;
	e.SendFoundPantheon(me, belief);  // as the popup sends it -> ResponseFoundPantheon
	const CvBeliefEntry* info = BeliefInfo(belief);
	const std::string name = BeliefName(belief);
	RecordText(RtAutoChoiceEvent::PANTHEON, blocking, (int)belief, byAI, info ? info->GetType() : "?", name, "Founded the pantheon " + name + " (chosen automatically)",
		byAI ? "CvReligionAI::ChoosePantheonBelief" : "first available");
	return true;
}

bool RtAutoChoice::ChooseReligion(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvGameReligions* religions = GC.getGame().GetGameReligions();
	CvReligionAI* ai = p.GetReligionAI();
	RtSession::BlockTarget t;
	s.CurrentBlocking(&t, me);  // the plot of the notification = the holy city (CvUnit::DoFoundReligion)
	const CvPlot* plot = (t.x >= 0 && t.y >= 0) ? GC.getMap().plot(t.x, t.y) : NULL;
	const CvCity* city = plot ? plot->getPlotCity() : NULL;
	if (!religions || !ai || !city) return false;
	const ReligionTypes religion = religions->GetReligionToFound(me);
	if (religion == NO_RELIGION)
	{
		Log(LOG_WARN, "autochoice: turn %d: no religion left to found", GC.getGame().getGameTurn());
		return false;
	}
	// SIMPLIFIED(SIMP-002): the AI picks the beliefs of the human's religion; original: the player picks in the religion popup.
	// The order and the functions of CvUnit::DoFoundReligion for an AI prophet (CvUnit.cpp:8577-8601).
	bool byAI = true;
	BeliefTypes b[4] = {NO_BELIEF, NO_BELIEF, NO_BELIEF, NO_BELIEF};
	int n = 0;
	if (!p.GetReligions()->HasCreatedPantheon()) b[n++] = PickBelief(ai->ChoosePantheonBelief(), religions->GetAvailablePantheonBeliefs(), byAI);
	b[n++] = PickBelief(ai->ChooseFollowerBelief(), religions->GetAvailableFollowerBeliefs(), byAI);
	b[n++] = PickBelief(ai->ChooseFounderBelief(), religions->GetAvailableFounderBeliefs(), byAI);
	if (p.GetPlayerTraits()->IsBonusReligiousBelief() && n < 4) b[n++] = ai->ChooseBonusBelief((int)b[0], (int)b[1], (int)b[2]);
	e.SendFoundReligion(me, religion, "", b[0], b[1], b[2], b[3], city->getX(), city->getY());  // as the popup sends it -> ResponseFoundReligion
	const CvReligionEntry* info = GC.getReligionInfo(religion);
	std::string name = info ? Localized(info->GetDescriptionKey()) : std::string();
	if (name.empty()) name = info ? info->GetType() : "?";
	std::string beliefs;
	for (int i = 0; i < 4; ++i)
		if (b[i] != NO_BELIEF) beliefs += (beliefs.empty() ? "" : ", ") + BeliefName(b[i]);
	RecordText(RtAutoChoiceEvent::RELIGION, blocking, (int)religion, byAI, info ? info->GetType() : "?", name,
		"Founded the religion " + name + " in " + Localized(city->getNameKey()) + " with " + beliefs + " (chosen automatically)",
		byAI ? "CvReligionAI::Choose*Belief" : "first available");
	return true;
}

bool RtAutoChoice::ChooseEnhancement(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvGameReligions* religions = GC.getGame().GetGameReligions();
	CvReligionAI* ai = p.GetReligionAI();
	if (!religions || !ai) return false;
	const ReligionTypes religion = religions->GetReligionCreatedByPlayer(me);
	if (religion == NO_RELIGION) return false;
	RtSession::BlockTarget t;
	s.CurrentBlocking(&t, me);
	// SIMPLIFIED(SIMP-002): the AI picks the enhancing beliefs for the human; original: the player picks in the enhance religion popup.
	// CvUnit::DoEnhanceReligion for an AI prophet (CvUnit.cpp:8740-8756): a follower belief and an enhancer belief.
	bool byAI = true;
	const BeliefTypes follower = PickBelief(ai->ChooseFollowerBelief(), religions->GetAvailableFollowerBeliefs(), byAI);
	const BeliefTypes enhancer = PickBelief(ai->ChooseEnhancerBelief(), religions->GetAvailableEnhancerBeliefs(), byAI);
	e.SendEnhanceReligion(me, religion, "", follower, enhancer, t.x, t.y);  // as the popup sends it -> ResponseEnhanceReligion
	const CvReligionEntry* info = GC.getReligionInfo(religion);
	std::string name = info ? Localized(info->GetDescriptionKey()) : std::string();
	if (name.empty()) name = info ? info->GetType() : "?";
	RecordText(RtAutoChoiceEvent::ENHANCEMENT, blocking, (int)religion, byAI, info ? info->GetType() : "?", name,
		"Enhanced the religion " + name + " with " + BeliefName(follower) + ", " + BeliefName(enhancer) + " (chosen automatically)",
		byAI ? "CvReligionAI::ChooseFollowerBelief/ChooseEnhancerBelief" : "first available");
	return true;
}

bool RtAutoChoice::ChooseReformation(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvGameReligions* religions = GC.getGame().GetGameReligions();
	CvReligionAI* ai = p.GetReligionAI();
	// ResponseFoundPantheon's condition for a reformation belief.
	if (!religions || !ai || religions->HasAddedReformationBelief(me) || !p.GetReligions()->HasCreatedReligion()) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the reformation belief for the human; original: the player picks in the reformation popup.
	bool byAI = true;
	const BeliefTypes belief = PickBelief(ai->ChooseReformationBelief(), religions->GetAvailableReformationBeliefs(), byAI);
	if (belief == NO_BELIEF) return false;
	e.SendFoundPantheon(me, belief);  // the popup uses the pantheon sender: ResponseFoundPantheon routes a reformation belief to AddReformationBelief
	const CvBeliefEntry* info = BeliefInfo(belief);
	const std::string name = BeliefName(belief);
	RecordText(RtAutoChoiceEvent::REFORMATION, blocking, (int)belief, byAI, info ? info->GetType() : "?", name, "Added the reformation belief " + name + " (chosen automatically)",
		byAI ? "CvReligionAI::ChooseReformationBelief" : "first available");
	return true;
}

bool RtAutoChoice::ChoosePromotion(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	CvPlayer& p = GET_PLAYER(e.GetActivePlayer());
	CvUnit* unit = NULL;
	int loop;
	for (CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
		if (u->isPromotionReady() && !u->isDelayedDeath()) { unit = u; break; }
	if (!unit) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the promotion for the human; original: the player picks on the promotion buttons.
	// CvUnit::AI_promote's loop (CvUnit.cpp:24851-24875): the canPromote promotion with the highest AI_promotionValue, lowest id on a tie.
	PromotionTypes best = NO_PROMOTION, firstPossible = NO_PROMOTION;
	int bestValue = 0;
	for (int i = 0; i < GC.getNumPromotionInfos(); ++i)
	{
		const PromotionTypes promotion = (PromotionTypes)i;
		if (!unit->canPromote(promotion, -1)) continue;
		if (firstPossible == NO_PROMOTION) firstPossible = promotion;
		const int value = unit->AI_promotionValue(promotion);
		if (value > bestValue) { bestValue = value; best = promotion; }
	}
	bool byAI = true;
	if (best == NO_PROMOTION)
	{
		// Fallback: AI_promote promotes nothing when every value is 0; the human's blocker needs one, so the first possible promotion.
		byAI = false;
		best = firstPossible;
	}
	if (best == NO_PROMOTION) return false;
	e.sendDoCommand(unit->GetID(), COMMAND_PROMOTION, (int)best, -1, false);  // as the promotion button sends it -> ResponseDoCommand
	const CvPromotionEntry* info = GC.getPromotionInfo(best);
	std::string name = info ? Localized(info->GetDescriptionKey()) : std::string();
	if (name.empty()) name = info ? info->GetType() : "?";
	RecordText(RtAutoChoiceEvent::PROMOTION, blocking, (int)best, byAI, info ? info->GetType() : "?", name,
		std::string("Promoted ") + unit->getName().GetCString() + ": " + name + " (chosen automatically)", byAI ? "CvUnit::AI_promotionValue" : "first possible");
	return true;
}

bool RtAutoChoice::ChooseGreatPerson(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	const bool faith = blocking == ENDTURN_BLOCKING_FAITH_GREAT_PERSON;
	if ((faith ? p.GetNumFaithGreatPeople() : p.GetNumFreeGreatPeople()) <= 0 || !p.getCapitalCity()) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the great person for the human; original: the player picks in the great person popup.
	UnitTypes unit = StrategicGreatPerson(p);
	bool byAI = true;
	if (unit == NO_UNIT)
	{
		// The AI takes none here. Fallback: the first of the gamecore's "no obvious strategic choice" order (CvPlayerTraits::ChooseMayaBoost).
		byAI = false;
		static const char* const order[] = {"UNIT_PROPHET", "UNIT_ENGINEER", "UNIT_WRITER", "UNIT_MERCHANT", "UNIT_SCIENTIST", "UNIT_ARTIST", "UNIT_MUSICIAN",
			"UNIT_GREAT_GENERAL", "UNIT_GREAT_ADMIRAL"};
		for (size_t i = 0; i < sizeof(order) / sizeof(order[0]) && unit == NO_UNIT; ++i)
		{
			if (i == 0 && !ProphetUseful(p)) continue;  // cannot found or enhance a religion: not a Great Prophet
			unit = UnitByType(order[i]);
		}
	}
	if (unit == NO_UNIT) return false;
	if (faith) e.sendFaithGreatPersonChoice(me, unit); else e.sendGreatPersonChoice(me, unit);  // -> ResponseFaithGreatPersonChoice / ResponseGreatPersonChoice
	const CvUnitEntry* info = GC.getUnitInfo(unit);
	const std::string name = UnitName(unit);
	RecordText(faith ? RtAutoChoiceEvent::FAITH_GREAT_PERSON : RtAutoChoiceEvent::FREE_GREAT_PERSON, blocking, (int)unit, byAI, info ? info->GetType() : "?", name,
		(faith ? "Great person bought with faith: " : "Free great person: ") + name + " (chosen automatically)",
		byAI ? "CvPlayerAI::AI_chooseFreeGreatPerson" : "first of the Maya order");
	return true;
}

bool RtAutoChoice::ChooseMaya(RtSession& s, int blocking)
{
	CvPlayer& p = GET_PLAYER(s.Engine().GetActivePlayer());
	CvPlayerTraits* traits = p.GetPlayerTraits();
	const int before = p.GetNumMayaBoosts();
	if (!traits || before <= 0 || !p.getCapitalCity()) return false;
	std::vector<int> ids;  // the units before: the great person is the new one (GetUnitBaktun is 0 for every unit in the first baktun)
	int loop;
	for (CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop)) ids.push_back(u->GetID());
	// SIMPLIFIED(SIMP-002): the AI picks the Long Count great person for the human; original: the player picks in the Maya popup.
	// ChooseMayaBoost chooses and applies in one call, as ResponseMayaBonusChoice does (see the header).
	traits->ChooseMayaBoost();
	if (p.GetNumMayaBoosts() >= before)
	{
		Log(LOG_WARN, "autochoice: turn %d: CvPlayerTraits::ChooseMayaBoost chose nobody", GC.getGame().getGameTurn());
		return false;
	}
	UnitTypes unit = NO_UNIT;
	for (CvUnit* u = p.firstUnit(&loop); u && unit == NO_UNIT; u = p.nextUnit(&loop))
	{
		bool known = false;
		for (size_t i = 0; i < ids.size() && !known; ++i) known = ids[i] == u->GetID();
		if (!known) unit = u->getUnitType();
	}
	const CvUnitEntry* info = GC.getUnitInfo(unit);
	const std::string name = unit == NO_UNIT ? std::string("?") : UnitName(unit);
	RecordText(RtAutoChoiceEvent::MAYA_GREAT_PERSON, blocking, (int)unit, true, info ? info->GetType() : "?", name, "Maya Long Count great person: " + name + " (chosen automatically)",
		"CvPlayerTraits::ChooseMayaBoost");
	return true;
}

bool RtAutoChoice::ChooseCapturedCity(RtSession& s)
{
	RtEngine& e = s.Engine();
	CvPlayer& p = GET_PLAYER(e.GetActivePlayer());
	CvCity* city = NULL;
	int loop;
	for (CvCity* c = p.firstCity(&loop); c; c = p.nextCity(&loop))
		if (c->IsIgnoreCityForHappiness() && !c->IsRazing()) { city = c; break; }  // set by CvPlayer::acquireCity for the popup (CvPlayer.cpp:3451)
	if (!city) return false;
	// SIMPLIFIED(SIMP-002): the AI decides the fate of the human's captured city; original: the player picks in CityCapturedPopup.
	// CvPlayerAI::AI_conquerCity (CvPlayerAI.cpp:319) without its liberation branches.
	const char* what;
	const char* rule;
	if (p.canRaze(city) && (p.IsEmpireUnhappy() || (GC.getMap().GetAIMapHint() & 2) ||
		(p.GetPlayerTraits()->GetRazeSpeedModifier() > 0 && p.getNumCities() >= 3 + (GC.getGame().getGameTurn() / 100))))
	{
		e.sendDoTask(city->GetID(), TASK_RAZE, -1, -1, false, false, false, false);  // as the popup's raze button sends it -> ResponseCityDoTask
		what = "Razed"; rule = "CvPlayerAI::AI_conquerCity: raze";
	}
	else if (city->getOriginalOwner() != p.GetID() || p.GetPlayerTraits()->IsNoAnnexing())
	{
		e.sendDoTask(city->GetID(), TASK_CREATE_PUPPET, -1, -1, false, false, false, false);  // the popup's puppet button -> ResponseCityDoTask
		what = "Made a puppet of"; rule = "CvPlayerAI::AI_conquerCity: puppet";
	}
	else
	{
		city->SetIgnoreCityForHappiness(false);  // nothing to send: the city simply stays ours
		what = "Kept"; rule = "CvPlayerAI::AI_conquerCity: keep";
	}
	const std::string name = Localized(city->getNameKey());
	RecordText(RtAutoChoiceEvent::CAPTURED_CITY, -1, city->GetID(), true, "CITY", name, std::string(what) + " the captured city " + name + " (chosen automatically)", rule);
	return true;
}

// ---- part 3 (F5) ----

bool RtAutoChoice::ChooseLeagueProposals(RtSession& s, int blocking)
{
	const PlayerTypes me = s.Engine().GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvLeague* league = ActiveLeague();
	CvLeagueAI* ai = p.GetLeagueAI();
	if (!league || !ai || !league->CanPropose(me)) return false;  // the state does not ask for it (any more)
	// SIMPLIFIED(SIMP-002): the AI proposes for the human in the World Congress; original: the player picks in the league overview.
	// DoProposals proposes directly (CvLeague::DoProposeEnact / DoProposeRepeal, the bodies of ResponseLeagueProposeEnact / Repeal), see the header.
	std::vector<int> known;
	{
		EnactProposalList enact = league->GetEnactProposals();
		for (EnactProposalList::iterator it = enact.begin(); it != enact.end(); ++it) known.push_back(it->GetID());
		RepealProposalList repeal = league->GetRepealProposals();
		for (RepealProposalList::iterator it = repeal.begin(); it != repeal.end(); ++it) known.push_back(it->GetID());
	}
	ai->DoProposals(league);
	std::string names;
	int first = -1, added = 0;
	{
		EnactProposalList enact = league->GetEnactProposals();
		for (EnactProposalList::iterator it = enact.begin(); it != enact.end(); ++it)
		{
			bool old = false;
			for (size_t i = 0; i < known.size() && !old; ++i) old = known[i] == it->GetID();
			if (old || it->GetProposalPlayer() != me) continue;
			if (first < 0) first = it->GetID();
			++added;
			names += (names.empty() ? "" : "; ") + PlainText(it->GetProposalName(false).c_str());
		}
		RepealProposalList repeal = league->GetRepealProposals();
		for (RepealProposalList::iterator it = repeal.begin(); it != repeal.end(); ++it)
		{
			bool old = false;
			for (size_t i = 0; i < known.size() && !old; ++i) old = known[i] == it->GetID();
			if (old || it->GetProposalPlayer() != me) continue;
			if (first < 0) first = it->GetID();
			++added;
			names += (names.empty() ? "" : "; ") + PlainText(it->GetProposalName(false).c_str());
		}
	}
	if (added == 0)
	{
		Log(LOG_WARN, "autochoice: turn %d: CvLeagueAI::DoProposals proposed nothing", GC.getGame().getGameTurn());
		return false;
	}
	RecordText(RtAutoChoiceEvent::LEAGUE_PROPOSAL, blocking, first, true, "LEAGUE_PROPOSAL", names, "Proposed in the World Congress: " + names + " (chosen automatically)",
		"CvLeagueAI::DoProposals");
	return true;
}

bool RtAutoChoice::ChooseLeagueVotes(RtSession& s, int blocking)
{
	const PlayerTypes me = s.Engine().GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvLeague* league = ActiveLeague();
	CvLeagueAI* ai = p.GetLeagueAI();
	if (!league || !ai || !league->CanVote(me)) return false;
	// SIMPLIFIED(SIMP-002): the AI casts the human's World Congress delegates; original: the player votes in the league overview.
	// DoVotes casts directly (CvLeague::DoVoteEnact / DoVoteRepeal, the bodies of ResponseLeagueVoteEnact / Repeal), see the header.
	const int before = league->GetRemainingVotesForMember(me);
	if (league->IsAnythingProposed()) ai->DoVotes(league); else ai->DoAbstainAllVotes(league);
	const int spent = before - league->GetRemainingVotesForMember(me);
	if (spent <= 0)
	{
		Log(LOG_WARN, "autochoice: turn %d: CvLeagueAI::DoVotes cast no delegate", GC.getGame().getGameTurn());
		return false;
	}
	std::string detail;
	{
		EnactProposalList enact = league->GetEnactProposals();
		for (EnactProposalList::iterator it = enact.begin(); it != enact.end(); ++it)
		{
			const int n = it->GetVoterDecision()->GetVotesCastByPlayer(me);
			if (n > 0) detail += (detail.empty() ? "" : "; ") + PlainText(it->GetProposalName(false).c_str()) + " x" + Format("%d", n);
		}
		RepealProposalList repeal = league->GetRepealProposals();
		for (RepealProposalList::iterator it = repeal.begin(); it != repeal.end(); ++it)
		{
			const int n = it->GetVoterDecision()->GetVotesCastByPlayer(me);
			if (n > 0) detail += (detail.empty() ? "" : "; ") + PlainText(it->GetProposalName(false).c_str()) + " x" + Format("%d", n);
		}
	}
	RecordText(RtAutoChoiceEvent::LEAGUE_VOTE, blocking, spent, true, "LEAGUE_VOTE", detail,
		Format("Cast %d World Congress delegate(s)", spent) + (detail.empty() ? std::string(", all abstained") : ": " + detail) + " (chosen automatically)", "CvLeagueAI::DoVotes");
	return true;
}

bool RtAutoChoice::ChooseArchaeology(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvPlayerCulture* culture = p.GetCulture();
	CvPlot* plot = NULL;
	CvUnit* unit = culture ? culture->GetNextDigCompleteArchaeologist(&plot) : NULL;
	if (!culture || !plot || !unit || p.GetNumArchaeologyChoices() <= 0)
	{
		if (p.GetNumArchaeologyChoices() > 0)
			Log(LOG_WARN, "autochoice: turn %d: a finished dig but no archaeologist at it (plot %s, %d choice(s) open)", GC.getGame().getGameTurn(), plot ? "found" : "none", p.GetNumArchaeologyChoices());
		return false;
	}
	// SIMPLIFIED(SIMP-002): the AI decides what to do with the human's finished dig; original: the player picks in the archaeology popup.
	ArchaeologyChoiceType choice = culture->GetArchaeologyChoice(plot);
	// The artifact and writing choices need a free great work slot that the AI function checks only for the art artifact slot; DoArchaeologyChoice
	// needs the housing city. Downgrade to what needs no slot.
	bool byAI = true;
	if ((choice == ARCHAEOLOGY_ARTIFACT_PLAYER1 || choice == ARCHAEOLOGY_ARTIFACT_PLAYER2) && !culture->HasAvailableGreatWorkSlot(CvTypes::getGREAT_WORK_SLOT_ART_ARTIFACT()))
	{
		choice = plot->HasWrittenArtifact() ? ARCHAEOLOGY_CULTURE_BOOST : ARCHAEOLOGY_LANDMARK;
		byAI = false;
	}
	else if (choice == ARCHAEOLOGY_ARTIFACT_WRITING && !culture->HasAvailableGreatWorkSlot(CvTypes::getGREAT_WORK_SLOT_LITERATURE()))
	{
		choice = ARCHAEOLOGY_CULTURE_BOOST;
		byAI = false;
	}
	e.SendArchaeologyChoice(me, unit->GetID(), choice);  // as the archaeology popup sends it -> ResponseArchaeologyChoice
	const char* what = "left the site";
	const char* type = "ARCHAEOLOGY_DO_NOTHING";
	switch (choice)
	{
	case ARCHAEOLOGY_LANDMARK: what = "built a landmark"; type = "ARCHAEOLOGY_LANDMARK"; break;
	case ARCHAEOLOGY_ARTIFACT_PLAYER1: what = "took the artifact"; type = "ARCHAEOLOGY_ARTIFACT_PLAYER1"; break;
	case ARCHAEOLOGY_ARTIFACT_PLAYER2: what = "took the artifact of the second civilization"; type = "ARCHAEOLOGY_ARTIFACT_PLAYER2"; break;
	case ARCHAEOLOGY_CULTURE_BOOST: what = "took a cultural renaissance"; type = "ARCHAEOLOGY_CULTURE_BOOST"; break;
	case ARCHAEOLOGY_ARTIFACT_WRITING: what = "took the written artifact"; type = "ARCHAEOLOGY_ARTIFACT_WRITING"; break;
	default: break;
	}
	RecordText(RtAutoChoiceEvent::ARCHAEOLOGY, blocking, (int)choice, byAI, type, what,
		Format("Excavated the site at (%d,%d): ", plot->getX(), plot->getY()) + what + " (chosen automatically)",
		byAI ? "CvPlayerCulture::GetArchaeologyChoice" : "CvPlayerCulture::GetArchaeologyChoice, no free slot");
	return true;
}

bool RtAutoChoice::ChooseGoody(RtSession& s)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	std::vector<RtUserInterface::PendingGoody>& pending = s.UI().PendingGoodies();
	for (;;)
	{
		// The first reward of this human (a network game keeps the rewards of every human in the one list; another human's stays for him).
		size_t at = 0;
		while (at < pending.size() && pending[at].player != (int)me) ++at;
		if (at == pending.size()) return false;
		const RtUserInterface::PendingGoody g = pending[at];
		pending.erase(pending.begin() + at);
		CvUnit* unit = p.getUnit(g.unit);
		CvPlot* plot = GC.getMap().plot(g.x, g.y);
		if (!unit || !plot || !unit->isHasPromotion((PromotionTypes)GC.getPROMOTION_GOODY_HUT_PICKER()))
		{
			Log(LOG_WARN, "autochoice: turn %d: the unit of a ruin reward popup is gone; reward dropped", GC.getGame().getGameTurn());
			continue;
		}
		// SIMPLIFIED(SIMP-002): the AI picks the ruin reward for the human (random, as for a unit without the picker promotion); original: the player picks
		// in the reward popup. The list of CvPlayer::doGoody (CvPlayer.cpp:8540-8571): the handicap's goodies that canReceiveGoody allows, repeats weigh more.
		CvHandicapInfo& hi = p.getHandicapInfo();
		std::vector<GoodyTypes> valid;
		for (int i = 0; i < hi.getNumGoodies(); ++i)
		{
			const GoodyTypes goody = (GoodyTypes)hi.getGoodies(i);
			bool seen = false;
			for (size_t j = 0; j < valid.size() && !seen; ++j) seen = valid[j] == goody;
			if (seen || p.canReceiveGoody(plot, goody, unit)) valid.push_back(goody);
		}
		if (valid.empty())
		{
			Log(LOG_WARN, "autochoice: turn %d: no ruin reward the unit can receive; reward dropped", GC.getGame().getGameTurn());
			continue;
		}
		const GoodyTypes goody = valid[GC.getGame().getJonRandNum((int)valid.size(), "Picking a Goody result")];
		e.sendGoodyChoice(me, g.x, g.y, goody, g.unit);  // as the popup sends it -> ResponseGoodyChoice
		std::string type;
		const std::string name = GoodyName(goody, type);
		RecordText(RtAutoChoiceEvent::GOODY_HUT, -1, (int)goody, true, type.c_str(), name, "Ancient ruin reward: " + name + " (chosen automatically)", "CvPlayer::doGoody random pick");
		return true;
	}
	return false;
}

bool RtAutoChoice::ChooseStealTech(RtSession& s, int blocking)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	CvPlayerTechs* techs = p.GetPlayerTechs();
	CvTechAI* ai = techs ? techs->GetTechAI() : NULL;
	CvPlayerEspionage* espionage = p.GetEspionage();
	if (!techs || !ai || !espionage) return false;
	// The victim: the notification's game data (CvEspionageClasses.cpp:849, eCityOwner); else the first civilization with a tech left to steal.
	RtSession::BlockTarget t;
	s.CurrentBlocking(&t, me);
	RtNotification note;
	const RtNotification* n = s.UI().FindNotificationOf(me, t.notification, &note) ? &note : NULL;
	PlayerTypes victim = NO_PLAYER;
	if (n && n->gameData >= 0 && n->gameData < MAX_MAJOR_CIVS && espionage->m_aiNumTechsToStealList[n->gameData] > 0) victim = (PlayerTypes)n->gameData;
	for (int i = 0; i < MAX_MAJOR_CIVS && victim == NO_PLAYER; ++i)
		if (espionage->m_aiNumTechsToStealList[i] > 0) victim = (PlayerTypes)i;
	if (victim == NO_PLAYER) return false;
	// SIMPLIFIED(SIMP-002): the AI picks the technology a spy steals for the human; original: the player picks in the steal tech popup. Highest CvTechAI
	// weight among the techs canStealTech allows, lowest id on a tie.
	TechTypes best = NO_TECH;
	int bestWeight = -1;
	for (int i = 0; i < GC.getNumTechInfos(); ++i)
	{
		const TechTypes tech = (TechTypes)i;
		if (!p.canStealTech(victim, tech)) continue;
		const int w = ai->GetWeight(tech);
		if (w > bestWeight) { bestWeight = w; best = tech; }
	}
	if (best == NO_TECH) return false;
	e.sendResearch(best, 0, victim, false);  // as the tech chooser sends it for a steal -> ResponseResearch
	const CvTechEntry* info = GC.getTechInfo(best);
	const std::string name = info ? Localized(info->GetDescriptionKey()) : std::string();
	const std::string civ = Localized(GET_PLAYER(victim).getCivilizationShortDescriptionKey());
	RecordText(RtAutoChoiceEvent::STEAL_TECH, blocking, (int)best, bestWeight > 0, info ? info->GetType() : "?", name.empty() ? std::string(info ? info->GetType() : "?") : name,
		"Stealing " + (name.empty() ? std::string("a technology") : name) + " from " + (civ.empty() ? std::string("a rival") : civ) + " (chosen automatically)",
		bestWeight > 0 ? "CvTechAI::GetWeight" : "first stealable");
	return true;
}

}  // namespace lekmod_portable
