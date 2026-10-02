// The server's events, used until an admin uploads others (PUT /admin/events, see README.md). IB3's ClashMob 2.0:
// Trials, ClashMobs and Aegis Tournaments (index.js has how each works).
//
// One section per event; its name becomes part of the event id, so keep it to letters, digits, "-" and "_". Stages
// are sections named after the event with ".1", ".2"...: each stage takes the event's lines, with its own in place of
// the same keys (a stage with reward lines replaces all of the event's). The server's own keys:
//   Type=ClashMob   Trial, ClashMob or Tournament
//   Days=7          how long the event runs (Hours= for less); it then starts again, everyone's progress reset.
//                   A tournament's stages share it equally
//   Goal=100        ClashMob: the goal for the whole mob (each stage has its own). Reaching it clears the stage
//   Score=Total     what a player's score counts: Total (all their plays together; ClashMobs) or Best (their best
//                   play; Trials and tournaments)
//   TopPercent=50   tournaments: the share of a stage's players who go on to the next stage
//   MaxScore=1      the most one play can add (else by BattleType: 1 boss, BossHealth damage, EndTime seconds...)
// Every other line goes into the event file the game downloads (SwordBattleEvent properties: see clashmob.cpp).
// Rewards: .RewardType= an eTouchRewardActor (TRA_Gold_Large, TRA_Chips_Small, TRA_GrabBag_Uber = the ClashMob Prize
// Wheel, TRA_Item_Fixed or TRA_Gem_Fixed with .RewardData= the item or gem), .RewardGoal= the score it needs. A
// ClashMob or tournament stage has one reward, given to everyone who played it once the stage is over and won (a
// tournament stage: to the players who went through).
export default `
[darkknight]
Type=ClashMob
Days=7
Title=The Dark Knight ClashMob
BattleType=BT_KillNBosses
BossObj=10ft_SnS_BlackKnight
BossLevel=10
BossScaledLevel=1.0
MapName=00_ClashMob_BaseScripting
SubMapName=cm_obelisk_art
QuestMapPin=MapPin_Obelisk_A

[darkknight.1]
Goal=50
Desc=A band of DARK KNIGHTS has overrun the Obelisk! In this first stage, the mob must kill 50 of them. Kill one to earn the reward.
.RewardType=TRA_Gold_Large
.RewardData=
.RewardGoal=1

[darkknight.2]
Goal=150
BossLevel=15
BossScaledLevel=1.2
Desc=Stronger DARK KNIGHTS have answered the call. The mob must kill 150 more. Kill one to earn the reward.
.RewardType=TRA_GrabBag_LargeGem
.RewardData=
.RewardGoal=1

[darkknight.3]
Goal=300
BossLevel=20
BossScaledLevel=1.5
Desc=The last of the DARK KNIGHTS are the deadliest. Kill 300 to drive them from the Obelisk for good!
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=1

[goliath]
Type=Trial
Days=7
Score=Best
Title=Clash with the MX-Goliath
Desc=The MX-GOLIATH has 100,000 health. Do as much damage as you can in 30 seconds!
BattleType=BT_Kill1Boss
BossObj=20ft_B_MX-Goliath
BossLevel=10
BossScaledLevel=1.0
BossHealth=100000
MaxPlays=10
.RewardType=TRA_Gold_Medium
.RewardData=
.RewardGoal=500
.RewardType=TRA_GrabBag_LargeGem
.RewardData=
.RewardGoal=2000
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=5000
MapName=00_ClashMob_BaseScripting
SubMapName=cm_dunes_art
QuestMapPin=MapPin_Dunes_A

[emberknight]
Type=Trial
Days=7
Score=Best
Title=Survive the Ember Knight
Desc=The EMBER KNIGHT cannot be beaten. Stay alive as long as you can!
BattleType=BT_TimeSurvival
BossObj=10ft_SnS_LavaLord
BossLevel=15
BossScaledLevel=1.5
BossHealth=10000000
EndTime=60
MaxPlays=10
.RewardType=TRA_Gold_Medium
.RewardData=
.RewardGoal=15
.RewardType=TRA_Gold_Large
.RewardData=
.RewardGoal=30
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=60
MapName=00_ClashMob_BaseScripting
SubMapName=C01_CM_Monastery_Art
QuestMapPin=MapPin_Monastary_A

[aegis]
Type=Tournament
Days=5
Score=Best
TopPercent=50
Title=Aegis Tournament
BattleType=BT_Kill1Boss
BossObj=10ft_SnS_Ashimar
BossLevel=20
BossScaledLevel=1.2
BossHealth=1000000
EndTime=30
MaxPlays=5
MapName=00_ClashMob_BaseScripting
SubMapName=cm_obelisk_art
QuestMapPin=MapPin_ThePit

[aegis.1]
Desc=Stage 1 of 5: do as much damage to ASHIMAR as you can in 30 seconds. Your best fight counts, and the top half go on to stage 2.
.RewardType=TRA_Chips_Small
.RewardData=
.RewardGoal=1

[aegis.2]
BossLevel=25
Desc=Stage 2 of 5: ASHIMAR grows stronger. Only the top half go on.
.RewardType=TRA_Chips_Medium
.RewardData=
.RewardGoal=1

[aegis.3]
BossLevel=30
Desc=Stage 3 of 5: the field is thinning. Only the top half go on.
.RewardType=TRA_Chips_Large
.RewardData=
.RewardGoal=1

[aegis.4]
BossLevel=35
Desc=Stage 4 of 5: one more stage stands between you and the final.
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=1

[aegis.5]
BossLevel=40
Desc=The final: the best of the tournament fight for ANARCHAX, a heavy weapon you can win nowhere else.
.RewardType=TRA_Item_Fixed
.RewardData=Sword_222
.RewardGoal=1
`;
