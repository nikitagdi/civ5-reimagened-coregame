// CvEnumSerialization (CvGameCoreDLLUtil), rewritten for the portable build: every enum travels as INT32.
// Generated from CvGameCoreDLLUtil/include/CvEnumSerialization.h; keep the list in sync with it.
#include "CvGameCoreDLLPCH.h"
#include "CvEnumSerialization.h"

namespace {
template <class E>
FDataStream& save_enum(FDataStream& s, const E& v)
{
	INT32 raw = static_cast<INT32>(v);
	s << raw;
	return s;
}
template <class E>
FDataStream& load_enum(FDataStream& s, E& v)
{
	INT32 raw = 0;
	s >> raw;
	// B4 gc-replica: an enum wider than 32 bits (NotificationTypes, PlayerOptionTypes) has 32-bit hash enumerators above INT32_MAX, positive here where MSVC
	// wraps them to negative ints; the save holds the 32-bit pattern, so it is read back unsigned (NO_* = -1 stays), or a loaded value matches no enumerator.
	if (sizeof(E) > sizeof(INT32) && raw < -1)
		v = static_cast<E>(static_cast<UINT32>(raw));
	else
		v = static_cast<E>(raw);
	return s;
}
}  // namespace

FDataStream& operator<<(FDataStream& s, const PlayerColorTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PlayerColorTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const PlayerOptionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PlayerOptionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ClimateTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ClimateTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const SeaLevelTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, SeaLevelTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const WorldSizeTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, WorldSizeTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const HandicapTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, HandicapTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameSpeedTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameSpeedTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const TurnTimerTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, TurnTimerTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const EraTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, EraTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CivilizationTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CivilizationTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeaderHeadTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeaderHeadTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ChatTargetTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ChatTargetTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const TeamTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, TeamTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const PlayerTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PlayerTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ReligionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ReligionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const PlayerVoteTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PlayerVoteTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const OrderTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, OrderTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const TaskTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, TaskTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CalendarTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CalendarTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const MissionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, MissionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CommandTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CommandTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const TechTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, TechTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const PolicyTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PolicyTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const NetContactTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, NetContactTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const SaveGameTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, SaveGameTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameMapTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameMapTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameMode& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameMode& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const SlotClaim& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, SlotClaim& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const SlotStatus& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, SlotStatus& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CivLoginStates& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CivLoginStates& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const VersionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, VersionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const BandwidthType& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, BandwidthType& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const VictoryTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, VictoryTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const SmallAwardTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, SmallAwardTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const AdvancedStartActionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, AdvancedStartActionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const UnitAITypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, UnitAITypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const VoteSourceTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, VoteSourceTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const VoteTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, VoteTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const BuildingClassTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, BuildingClassTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ColorTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ColorTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const InterfaceMessageTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, InterfaceMessageTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const DirectionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, DirectionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const UnitTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, UnitTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const BuildingTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, BuildingTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ReplayMessageTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ReplayMessageTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ResourceTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ResourceTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const MissionAITypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, MissionAITypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ActivityTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ActivityTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const AutomateTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, AutomateTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameOptionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameOptionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ButtonPopupTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ButtonPopupTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const UnitClassTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, UnitClassTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const MinorCivTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, MinorCivTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ArtStyleTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ArtStyleTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const DiploCommentTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, DiploCommentTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameStateTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameStateTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const BuildTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, BuildTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CultureLevelTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CultureLevelTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const TacticalAIMoveTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, TacticalAIMoveTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CitySpecializationTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CitySpecializationTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const InvisibleTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, InvisibleTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const PolicyBranchTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, PolicyBranchTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const EndTurnBlockingTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, EndTurnBlockingTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const NotificationTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, NotificationTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const FromUIDiploEventTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, FromUIDiploEventTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const RouteTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, RouteTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ImprovementTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ImprovementTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const CityAIFocusTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, CityAIFocusTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GreatPeopleDirectiveTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GreatPeopleDirectiveTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const DiploUIStateTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, DiploUIStateTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeaderheadAnimationTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeaderheadAnimationTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const GameExecutableTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, GameExecutableTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const BeliefTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, BeliefTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const StorageLocation& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, StorageLocation& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeagueTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeagueTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeagueSpecialSessionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeagueSpecialSessionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeagueNameTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeagueNameTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const LeagueProjectTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, LeagueProjectTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ResolutionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ResolutionTypes& v) { return load_enum(s, v); }
FDataStream& operator<<(FDataStream& s, const ResolutionDecisionTypes& v) { return save_enum(s, v); }
FDataStream& operator>>(FDataStream& s, ResolutionDecisionTypes& v) { return load_enum(s, v); }

namespace FSerialization {
std::string toString(const DirectionTypes& v) { return toString(static_cast<int>(v)); }
std::string toString(const ActivityTypes& v) { return toString(static_cast<int>(v)); }
}  // namespace FSerialization
