#include "TabletPublisher.h"
#include <vector>
#include "LapReportGenerator.h"   // LapStats, ReportResult, getValidLaps
#include "MotecSetupAnalyzer.h"    // buildAverageLapData (free function)
#include "ReportJsonWriter.h"      // reportjson::

TabletPublisher::TabletPublisher(int defaultPort) : server_(defaultPort) {}

bool TabletPublisher::isRunning() const { return server_.isRunning(); }
void TabletPublisher::stopServer() { if (server_.isRunning()) server_.stop(); }
void TabletPublisher::setPort(int port) { server_.setPort(port); }
int  TabletPublisher::activePort() const { return server_.port(); }
bool TabletPublisher::startServer() { return server_.start(); }

// Live pit-exit status frame: car/track/lap appear as soon as "Leaving Pits" is shown.
// Reuses the report version counter so polling tablets never re-render a stale payload.
void TabletPublisher::publishLiveStatus(const std::string& vehicleNameUtf8,
                                        const std::string& trackLocationUtf8, int lapCount)
{
    if (!server_.isRunning()) return;

    ++version_;
    std::string json = "{\"version\":" + std::to_string(version_) + ",\"meta\":{\"hasData\":true,\"carName\":\"" + reportjson::jsonEscape(vehicleNameUtf8) + "\",\"trackName\":\"" + reportjson::jsonEscape(trackLocationUtf8) + "\",\"lapCount\":"
        + (lapCount > 0 ? std::to_string(lapCount).c_str() : "null") + ",\"onTrack\":true},\"sections\":{},\"lapStats\":null}";
    server_.setPayload(json);
}

// Push a fresh JSON payload after every analysis. No-op when the server is not running;
// the version counter lets polling clients detect changes without re-rendering identical data.
void TabletPublisher::publishReport(const ReportResult& result, const std::vector<LapStats>& laps)
{
    if (!server_.isRunning()) return;

    LapStats avg = buildAverageLapData(laps);
    int included = (int)LapReportGenerator::getValidLaps(laps).size();
    ++version_;
    server_.setPayload(reportjson::buildReportJson(result, avg, included, version_));
}
