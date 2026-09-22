#include "progression/game/Persistence.h"

#include "progression/game/Log.h"
#include "progression/game/Service.h"

namespace fp::game
{

void WriteRecords(SKSE::SerializationInterface *intfc)
{
    std::size_t written = 0;
    for (const CoSaveRecord &record : SaveRecords())
    {
        if (!intfc->OpenRecord(record.type, record.version) ||
            (!record.payload.empty() &&
             !intfc->WriteRecordData(record.payload.data(), static_cast<std::uint32_t>(record.payload.size()))))
        {
            log::save.error("a record of type {:08X} could not be written", record.type);
            continue;
        }
        ++written;
    }
    log::save.info("saved {} record(s)", written);
}

void ReadRecords(const std::vector<CoSaveRecord> &records)
{
    LoadRecords(records);
}

void RevertRecords()
{
    Revert();
}

} // namespace fp::game
