// Resume after auto-sleep or a low-battery shutdown (as CGB-Bit does): main.cpp stores a snapshot
// of the game then and resumes it once at the next boot. Shared with tools/smoke_test.cpp, which
// runs it. Include game.h first.
#pragma once

// Whether shutdown stores the game: in a level (the main menu has none loaded), playing it or in
// its game menu, where a player who walks away leaves it.
inline bool resumable(){
	return SFG_currentLevel.levelPointer
		&& (SFG_game.state == SFG_GAME_STATE_PLAYING || SFG_game.state == SFG_GAME_STATE_MENU);
}

// Resumes the snapshot load(game, player, level) reads (false: none, or unreadable). Mod 12's
// SFG_quickLoad restores it (level clock rebased to this boot, unlocked levels and settings kept),
// so it passes through the quicksave variables; loadQuickSave() then reads the player's own
// quicksave back, and with none there's none (frame 0). Call after setting SFG_game.frameTime.
template<class Load, class LoadQuickSave> bool resumeGame(Load load, LoadQuickSave loadQuickSave){
	const bool ok = load(SFG_quickSaveGame, SFG_quickSavePlayer, SFG_quickSaveCurrentLevel);
	if(ok) SFG_quickLoad();
	SFG_quickSaveGame.frame = 0;
	loadQuickSave();
	return ok;
}
