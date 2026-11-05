// rt_autochoice: choices the human has no screen for in B1, made for the human by the gamecore's own AI.
// SIMPLIFIED(SIMP-002): the AI picks research, policies and the ideology for the human; original: the player picks in TechPopup /
// SocialPolicyPopup / ChooseIdeologyPopup.
// Part 1 (task F3): technology and free technology (ENDTURN_BLOCKING_RESEARCH, FREE_TECH; flag auto_choices.research), policy, policy
// branch, free policy and tenet (POLICY, FREE_POLICY; flag auto_choices.policy; "choose only": no ideology switch), ideology
// (CHOOSE_IDEOLOGY; flag auto_choices.ideology). RtSession::Pump calls Run() after every UpdateTestEndTurn; with a flag off the blocker
// stays and the turn waits for the human (WAITING_HUMAN with that blocker). Each choice becomes an AUTO_CHOICE event in a queue the
// caller drains (PopEvent); the ABI emits them elsewhere.
//
// Choice rules:
//  - technology: CvTechAI::ChooseNextTech(player, bFree) of the human's own tech AI (personality flavors all DEFAULT_FLAVOR_VALUE for
//    a human, CvFlavorManager::Init); it draws from the synchronous RNG as it does for the AI. When it returns nothing valid (total weight
//    0), the cheapest tech the player can research (for free) by CvPlayerTechs::GetResearchCost, lowest id on a tie. Sent as the tech
//    popup sends it: gDLL->sendResearch(tech, free ? GetNumFreeTechs() : 0, NO_PLAYER, false) -> ResponseResearch.
//  - policy / branch / free policy / tenet: CvPolicyAI::ChooseNextPolicy(player) of the human's own policy AI, i.e. one step of
//    CvPlayerPolicies::DoPolicyAI without its CvPolicyAI::DoConsiderIdeologySwitch. Sent as the policy screen sends it:
//    gDLL->sendUpdatePolicies(true, policy, true) / (false, branch, true) -> ResponseUpdatePolicies. NO_POLICY: the blocker stays.
//  - ideology: CvPlayerPolicies::DoChooseIdeology -> CvPolicyAI::DoChooseIdeology. The function is not pure: it unlocks the chosen
//    branch itself with SetPolicyBranchUnlocked(branch, true, false), which is the whole body of ResponseIdeologyChoice, so the choice is
//    applied by that call (between two Update() slices, as a loopback delivery would be) and not sent again.
//
// Part 2 (task F4; flags auto_choices.religion, promotion, great_person, captured_city):
//  - pantheon (ENDTURN_BLOCKING_FOUND_PANTHEON; religion): CvReligionAI::ChoosePantheonBelief of the human's own religion AI; when it returns
//    nothing (no belief scored above 0) or a belief that is not available, the first of CvGameReligions::GetAvailablePantheonBeliefs.
//    Sent as the pantheon popup sends it: gDLL->SendFoundPantheon(player, belief) -> ResponseFoundPantheon.
//  - religion (FOUND_RELIGION; religion): the religion is CvGameReligions::GetReligionToFound, the beliefs in the order the gamecore's own
//    prophet code picks them for the AI (CvUnit::DoFoundReligion, CvUnit.cpp:8577-8601): pantheon belief when the player has no pantheon,
//    follower, founder, and the bonus belief for a civilization with IsBonusReligiousBelief, each by CvReligionAI::Choose*Belief (first available
//    belief when it returns none). City = the notification's plot. Sent as the religion popup sends it: SendFoundReligion(..., "", b0..b3, x, y).
//  - enhancement (ENHANCE_RELIGION; religion): follower and enhancer belief (CvUnit::DoEnhanceReligion, CvUnit.cpp:8740-8756) of the religion
//    the player founded; SendEnhanceReligion(player, religion, "", follower, enhancer, x, y).
//  - reformation (ADD_REFORMATION_BELIEF; religion): CvReligionAI::ChooseReformationBelief; sent as the reformation popup sends it, with
//    SendFoundPantheon(player, belief) (ResponseFoundPantheon routes a reformation belief to AddReformationBelief).
//  - promotion (UNIT_PROMOTION; promotion): for the first unit with a promotion ready, the promotion CvUnit::AI_promote would take first (the
//    canPromote promotion with the highest CvUnit::AI_promotionValue, lowest id on a tie); when no promotion scores above 0, the first canPromote
//    one. AI_promote applies it itself and recurses, so the choice is made here (one per step, the loop of AI_promote is the blocker coming
//    back) and sent as the promotion button sends it: sendDoCommand(unit, COMMAND_PROMOTION, promotion, -1, false).
//  - free great person (FREE_ITEMS; great_person) and faith great person (FAITH_GREAT_PERSON; great_person): the rule of
//    CvPlayerAI::AI_chooseFreeGreatPerson (Engineer for a wonder-competitive early game, else by the active grand strategy: General / Artist /
//    Merchant / Scientist), which has no separable chooser. When it names nobody the AI takes none; the human must, so then the first of the
//    gamecore's "no obvious strategic choice" order of CvPlayerTraits::ChooseMayaBoost (Prophet, Engineer, Writer, Merchant, Scientist, Artist,
//    Musician, Great General, Great Admiral; the Prophet is skipped when the player can neither found nor enhance a religion). Sent as the popups send it: sendGreatPersonChoice / sendFaithGreatPersonChoice.
//    (CvReligionAI::GetDesiredFaithGreatPerson is private; the AI itself uses AI_chooseFreeGreatPerson for a faith great person too.)
//  - Maya Long Count great person (MAYA_LONG_COUNT; great_person): CvPlayerTraits::ChooseMayaBoost, which chooses and applies in one call
//    (spawns the person, SetUnitBaktun, ChangeNumMayaBoosts(-1)): exactly the body of ResponseMayaBonusChoice, so it is called directly (as the
//    ideology is), between two slices. The UI path would be sendMayaBonusChoice.
//  - captured city (no blocker: the city keeps IsIgnoreCityForHappiness until annexed / puppeted / razed; captured_city): the decision of
//    CvPlayerAI::AI_conquerCity without its liberation branches: raze (sendDoTask TASK_RAZE) when canRaze and the empire is unhappy / the map
//    hint asks / the civilization razes fast, else puppet (sendDoTask TASK_CREATE_PUPPET) when the city was not originally ours or the
//    civilization cannot annex, else keep (flag cleared).
//    SIMPLIFIED(SIMP-002): no auto-liberation in B1; the liberation branches of AI_conquerCity (DoLiberatePlayer / DoPossibleMinorLiberation) are not taken,
//    the human never liberates a city automatically. AI_conquerCity decides and acts in one call, so its rule is repeated here to send.
// Part 3 (task F5; flags auto_choices.league, archaeology, goody_hut, steal_tech):
//  - World Congress proposals (ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS; league) and votes (ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES; league):
//    CvLeagueAI::DoProposals(league) / DoVotes(league) of the human's own league AI, the functions the gamecore runs for an AI member
//    (CvGameLeagues::DoPlayerTurn, CvVotingClasses.cpp:7423 / 7460). They decide and apply in one call (CvLeague::DoProposeEnact / DoProposeRepeal /
//    DoVoteEnact / DoVoteRepeal, the bodies of the ResponseLeague* handlers), so they are called directly between two slices, as the ideology is; the UI
//    path would be SendLeagueProposeEnact / ProposeRepeal / VoteEnact / VoteRepeal / VoteAbstain. They draw from the synchronous RNG. When the
//    member can still propose / vote afterwards (the AI function found nothing), the vote is abstained with DoAbstainAllVotes (as the gamecore does
//    for an AI that cannot decide, CvVotingClasses.cpp:8323); a proposal that stays possible is left to the human (the attempt limit).
//  - archaeology (ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY; archaeology): CvPlayerCulture::GetArchaeologyChoice(plot of the next finished dig), the AI's own rule
//    (landmark / artifact / writing / culture boost). Its artifact and writing choices need a free great work slot the human's empire may lack for
//    the writing slot, so the choice is downgraded to the culture boost / landmark in that case (DoArchaeologyChoice would dereference no city).
//    Sent as the archaeology popup sends it: Lua Network.SendArchaeologyChoice(player, unit index, choice), an exe-only message (RtEngine::SendArchaeologyChoice)
//    -> ResponseArchaeologyChoice.
//  - ruin reward (no blocker: CvPlayer::doGoody opens BUTTONPOPUP_CHOOSE_GOODY_HUT_REWARD for a unit with the goody hut picker promotion; RtUserInterface
//    records it as a pending goody; goody_hut): the choice the gamecore makes for a human without the promotion (CvPlayer::doGoody, CvPlayer.cpp:8540-8571):
//    the goodies of the handicap's list that CvPlayer::canReceiveGoody allows, one drawn at random (synchronous RNG, duplicates in the list weigh more).
//    Sent as the popup sends it: gDLL->sendGoodyChoice(player, x, y, goody, unit) -> ResponseGoodyChoice.
//  - tech steal (ENDTURN_BLOCKING_STEAL_TECH; steal_tech): of the techs the human may take from the victim (the notification's player;
//    CvPlayer::canStealTech) the one with the highest CvTechAI weight (CvTechAI::GetWeight, the weights ChooseNextTech draws from; lowest id on a
//    tie, so with all weights 0 the first), no RNG. Sent as the tech chooser sends it: gDLL->sendResearch(tech, 0, victim, false) -> ResponseResearch.
//
// STUCK: a choice blocker whose auto-choice flag is off, or that the choice could not clear, is WAITING_HUMAN while a human can answer.
// When the session has no human input (RtSession::NoHumanInput: the setup's autoplay_human, or SetNoHumanInput(true) by a host that cannot answer)
// nobody will, so RtSession::Pump returns STUCK with StuckReason(): the blocker, and the flag that would resolve it.
//
// Every rule only acts while the state still asks for the choice (no current research; free techs left; culture for a policy or a free
// policy / tenet; time to choose an ideology and none chosen), at most kMaxAttempts times per turn, human and blocker.
// Network game (N3.14): RtSession::NetworkFrame calls Run(session, human) for every human whose turn is active, under ActiveAs(human), so each choice, its
// attempt counters, its AUTO_CHOICE event (player = the human) and the ruin rewards (the session's list holds the rewards of all humans) are that human's. Flags are shared.
#pragma once

#include <deque>
#include <string>

#include "rt_setup.h"

namespace lekmod_portable {

class RtSession;

struct RtAutoChoiceEvent
{
	enum Kind
	{
		RESEARCH,       // the next research (ENDTURN_BLOCKING_RESEARCH)
		FREE_TECH,      // a free technology (ENDTURN_BLOCKING_FREE_TECH)
		POLICY,         // a policy or tenet adopted (POLICY or FREE_POLICY)
		POLICY_BRANCH,  // a policy branch opened (POLICY or FREE_POLICY)
		IDEOLOGY,       // the ideology (ENDTURN_BLOCKING_CHOOSE_IDEOLOGY)
		// part 2 (F4); append only, the values are in the ABI header
		PANTHEON,           // a pantheon belief (ENDTURN_BLOCKING_FOUND_PANTHEON); id = BeliefTypes
		RELIGION,           // a religion founded (ENDTURN_BLOCKING_FOUND_RELIGION); id = ReligionTypes
		ENHANCEMENT,        // a religion enhanced (ENDTURN_BLOCKING_ENHANCE_RELIGION); id = ReligionTypes
		REFORMATION,        // a reformation belief (ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF); id = BeliefTypes
		PROMOTION,          // a unit promotion (ENDTURN_BLOCKING_UNIT_PROMOTION); id = PromotionTypes
		FREE_GREAT_PERSON,  // a free great person (ENDTURN_BLOCKING_FREE_ITEMS); id = UnitTypes
		FAITH_GREAT_PERSON, // a great person bought with faith (ENDTURN_BLOCKING_FAITH_GREAT_PERSON); id = UnitTypes
		MAYA_GREAT_PERSON,  // the Maya Long Count great person (ENDTURN_BLOCKING_MAYA_LONG_COUNT); id = UnitTypes
		CAPTURED_CITY,      // the fate of a captured city (no blocker); id = the city id; text says raze / puppet / keep
		// part 3 (F5); append only
		LEAGUE_PROPOSAL,    // World Congress proposals made (ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS); id = the id of the first new proposal
		LEAGUE_VOTE,        // World Congress delegates cast (ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES); id = the number of delegates spent
		ARCHAEOLOGY,        // a finished dig (ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY); id = ArchaeologyChoiceType
		GOODY_HUT,          // a ruin reward (no blocker; pending popup); id = GoodyTypes
		STEAL_TECH          // a technology stolen by a spy (ENDTURN_BLOCKING_STEAL_TECH); id = TechTypes
	};
	Kind kind;
	int turn;            // game turn of the choice
	int player;          // the human
	int blocking;        // EndTurnBlockingTypes that asked for it
	int id;              // TechTypes / PolicyTypes / PolicyBranchTypes
	bool ai;             // true: the gamecore AI function chose; false: the documented fallback (cheapest tech)
	std::string type;    // "TECH_POTTERY", "POLICY_LIBERTY", "POLICY_BRANCH_FREEDOM"
	std::string name;    // localized description ("Pottery")
	std::string text;    // journal line ("Researching Pottery (chosen automatically)")
	std::string rule;    // which rule chose: "CvTechAI::ChooseNextTech", "cheapest", "CvPolicyAI::ChooseNextPolicy", "CvPolicyAI::DoChooseIdeology"
	RtAutoChoiceEvent() : kind(RESEARCH), turn(-1), player(-1), blocking(-1), id(-1), ai(true) {}
};

class RtAutoChoice
{
public:
	// Per turn, human and blocker: a choice that did not clear the blocker is repeated at most this many times, then the blocker stays.
	static const int kMaxAttempts = 4;
	// Promotions come one per step and a turn can have many ready units; captured cities likewise.
	static const int kMaxPromotionAttempts = 64;
	static const int kMaxCapturedCityAttempts = 16;
	static const int kMaxStealAttempts = 16;
	static const int kMaxGoodyAttempts = 16;

	RtAutoChoice();

	// After UpdateTestEndTurn (RtSession::Pump). Acts on the active human's blocker when its flag is on. True when a choice was made (a
	// command is queued or the state changed): the caller must run another slice before it reports WAITING_HUMAN.
	bool Run(RtSession& session);
	// The same for human `who` of a network game, as that human (ActiveAs); the attempt counters are per human.
	bool Run(RtSession& session, PlayerTypes who);

	// The flags in use: the setup's auto_choices (RtSetup), copied on the first Run; SetFlags replaces them (tests, a later ABI call).
	const RtAutoChoices& Flags(const RtSession& session);
	void SetFlags(const RtAutoChoices& flags);

	// AUTO_CHOICE events not taken yet, oldest first.
	size_t PendingEvents() const { return m_events.size(); }
	bool PopEvent(RtAutoChoiceEvent& out);
	// Choices made since the start (events taken or not).
	int ChoiceCount() const { return m_choices; }

	static const char* KindName(int kind);

	// Why a choice blocker cannot be left to the human (RtSession::Pump's STUCK reason when there is no human input): names the blocker and the
	// auto_choices flag that would resolve it. Empty when `blocking` is not a blocker rt_autochoice has a flag for (units, production, ...).
	std::string StuckReason(RtSession& session, int blocking);
	// The auto_choices.<name> flag of a blocker, NULL when none.
	static const char* FlagName(int blocking);
	// Ruin rewards (pop-ups) waiting for the human because auto_choices.goody_hut is off.
	// `who` NO_PLAYER: all of them.
	int PendingGoodies(RtSession& session, PlayerTypes who = NO_PLAYER);

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	bool ChooseTech(RtSession& s, int blocking, bool free);
	bool ChoosePolicy(RtSession& s, int blocking);
	bool ChooseIdeology(RtSession& s, int blocking);
	bool ChoosePantheon(RtSession& s, int blocking);
	bool ChooseReligion(RtSession& s, int blocking);
	bool ChooseEnhancement(RtSession& s, int blocking);
	bool ChooseReformation(RtSession& s, int blocking);
	bool ChoosePromotion(RtSession& s, int blocking);
	bool ChooseGreatPerson(RtSession& s, int blocking);  // FREE_ITEMS and FAITH_GREAT_PERSON
	bool ChooseMaya(RtSession& s, int blocking);
	bool ChooseCapturedCity(RtSession& s);
	bool ChooseLeagueProposals(RtSession& s, int blocking);
	bool ChooseLeagueVotes(RtSession& s, int blocking);
	bool ChooseArchaeology(RtSession& s, int blocking);
	bool ChooseGoody(RtSession& s);
	bool ChooseStealTech(RtSession& s, int blocking);
	void Record(RtAutoChoiceEvent::Kind kind, int blocking, int id, bool ai, const char* type, const char* descKey, const char* textFmt, const char* rule);
	void RecordText(RtAutoChoiceEvent::Kind kind, int blocking, int id, bool ai, const char* type, const std::string& name, const std::string& text, const char* rule);

	RtAutoChoices m_flags;
	bool m_flagsSet;
	std::deque<RtAutoChoiceEvent> m_events;
	int m_choices;
	// Attempt counters per human (N3.14: in a network game several humans have the same blocker in the same turn; one shared counter
	// ran out for the fifth human), in m_by; rt_save.cpp saves them (runtime version 3).
	struct Counters
	{
		int turn;
		int attempts[64];                   // per EndTurnBlockingTypes, this turn; slot kCapturedSlot is the captured city
		bool gaveUpLogged[64];
	};
	Counters m_by[MAX_PLAYERS];
	static const int kCapturedSlot = 62;
	static const int kGoodySlot = 61;
};

}  // namespace lekmod_portable
