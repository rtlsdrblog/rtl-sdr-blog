/* Stub MscHandler - audio processing not needed for time sync */
#include "msc-handler.h"

MscHandler::MscHandler(const DABParams& p, bool) : bitsperBlock(2 * p.K) {}
void MscHandler::processMscBlock(const softbit_t*, int16_t) {}
void MscHandler::stopProcessing() {}
bool MscHandler::addSubchannel(ProgrammeHandlerInterface&, AudioServiceComponentType, const std::string&, const Subchannel&) { return false; }
bool MscHandler::removeSubchannel(const Subchannel&) { return false; }
