/*
 *	Copyright notice:
 *	Copyright © Members of the EMI Collaboration, 2010.
 *
 *	See www.eu-emi.eu for details on the copyright holders
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 *
 */

#include <boost/lexical_cast.hpp>
#include <boost/tokenizer.hpp>
#include <boost/regex.hpp>
#include <map>
#include <error.h>
#include <logger.h>
#include <soci/mysql/soci-mysql.h>
#include <mysql/mysql.h>
#include <random>
#include <signal.h>
#include <stdint.h>
#include <sys/param.h>
#include <unistd.h>
#include "MySqlAPI.h"
#include "MySqlMonitoring.h"
#include "sociConversions.h"
#include "queue_updater.h"
#include "DbUtils.h"

using namespace FTS3_COMMON_NAMESPACE;
using namespace db;


static unsigned getHashedId(void)
{
    static __thread std::mt19937 *generator = NULL;
    if (!generator)
        {
            generator = new std::mt19937(clock());
        }
#if __cplusplus <= 199711L
    std::uniform_int<unsigned> distribution(0, UINT16_MAX);
#else
    std::uniform_int_distribution<unsigned> distribution(0, UINT16_MAX);
#endif

    return distribution(*generator);
}



bool MySqlAPI::getChangedFile (std::string source, std::string dest, double rate, double& rateStored, double thr, double& thrStored, double retry, double& retryStored, int active, int& activeStored, int throughputSamples, int& throughputSamplesStored)
{
    bool returnValue = false;

    if(thr == 0 || rate == 0 || active == 0)
        return returnValue;

    if(filesMemStore.empty())
        {
            boost::tuple<std::string, std::string, double, double, double, int, int> record(source, dest, rate, thr, retry, active, throughputSamples);
            filesMemStore.push_back(record);
        }
    else
        {
            bool found = false;
            std::vector< boost::tuple<std::string, std::string, double, double, double, int, int> >::iterator itFind;
            for (itFind = filesMemStore.begin(); itFind < filesMemStore.end(); ++itFind)
                {
                    boost::tuple<std::string, std::string, double, double, double, int, int>& tupleRecord = *itFind;
                    std::string sourceLocal = boost::get<0>(tupleRecord);
                    std::string destLocal = boost::get<1>(tupleRecord);
                    if(sourceLocal == source && destLocal == dest)
                        {
                            found = true;
                            break;
                        }
                }
            if (!found)
                {
                    boost::tuple<std::string, std::string, double, double, double, int, int> record(source, dest, rate, thr, retry, active, throughputSamples);
                    filesMemStore.push_back(record);
                }

            std::vector< boost::tuple<std::string, std::string, double, double, double, int, int> >::iterator it =  filesMemStore.begin();
            while (it != filesMemStore.end())
                {
                    boost::tuple<std::string, std::string, double, double, double, int, int>& tupleRecord = *it;
                    std::string sourceLocal = boost::get<0>(tupleRecord);
                    std::string destLocal = boost::get<1>(tupleRecord);
                    double rateLocal = boost::get<2>(tupleRecord);
                    double thrLocal = boost::get<3>(tupleRecord);
                    double retryThr = boost::get<4>(tupleRecord);
                    int activeLocal = boost::get<5>(tupleRecord);
                    int throughputSamplesLocal = boost::get<6>(tupleRecord);

                    if(sourceLocal == source && destLocal == dest)
                        {
                            retryStored = retryThr;
                            thrStored = thrLocal;
                            rateStored = rateLocal;
                            activeStored = activeLocal;

                            if(thr < thrLocal)
                                {
                                    throughputSamplesLocal += 1;
                                }
                            else if(thr >= thrLocal && throughputSamplesLocal > 0)
                                {
                                    throughputSamplesLocal -= 1;
                                }
                            else
                                {
                                    throughputSamplesLocal = 0;
                                }

                            if(throughputSamplesLocal == 3)
                                {
                                    throughputSamplesStored = throughputSamplesLocal;
                                    throughputSamplesLocal = 0;
                                }


                            if(rateLocal != rate || thrLocal != thr || retry != retryThr)
                                {
                                    it = filesMemStore.erase(it);
                                    boost::tuple<std::string, std::string, double, double, double, int, int> record(source, dest, rate, thr, retry, active, throughputSamplesLocal);
                                    filesMemStore.push_back(record);
                                    returnValue = true;
                                    break;
                                }
                            break;
                        }
                    else
                        {
                            ++it;
                        }
                }
        }

    return returnValue;
}


MySqlAPI::MySqlAPI(): poolSize(10), connectionPool(NULL), hostname(getFullHostname())
{
    // Pass
}



MySqlAPI::~MySqlAPI()
{
    if(connectionPool)
        {
            for (size_t i = 0; i < poolSize; ++i)
                {
                    soci::session& sql = (*connectionPool).at(i);
                    sql << "select concat('KILL ',id,';') from information_schema.processlist where user=:username", soci::use(username_);
                }
        }
}


static void getHostAndPort(const std::string& conn, std::string* host, int* port)
{
    host->clear();
    *port = 0;

    std::string remaining;

    size_t close;
    if (conn.size() > 0 && conn[0] == '[' && (close = conn.find(']')) != std::string::npos)
        {
            host->assign(conn.substr(1, close - 1));
            remaining = conn.substr(close + 1);
        }
    else
        {
            size_t colon = conn.find(':');
            if (colon == std::string::npos)
                {
                    host->assign(conn);
                }
            else
                {
                    host->assign(conn.substr(0, colon));
                    remaining = conn.substr(colon);
                }
        }

    if (remaining[0] == ':')
        {
            *port = atoi(remaining.c_str() + 1);
        }
}



void MySqlAPI::init(std::string username, std::string password, std::string connectString, int pooledConn)
{
    std::ostringstream connParams;
    std::string host, db;
    int port;

    try
        {
            connectionPool = new soci::connection_pool(pooledConn);

            // From connectString, get host and db
            size_t slash = connectString.find('/');
            if (slash != std::string::npos)
                {
                    getHostAndPort(connectString.substr(0, slash), &host, &port);
                    db   = connectString.substr(slash + 1, std::string::npos);

                    connParams << "host='" << host << "' "
                               << "db='" << db << "' ";

                    if (port != 0)
                        connParams << "port=" << port << " ";
                }
            else
                {
                    connParams << "db='" << connectString << "' ";
                }
            connParams << " ";

            // Build connection string
            connParams << "user='" << username << "' "
                       << "pass='" << password << "'";
            username_ = username;

            std::string connStr = connParams.str();

            // Connect
            static const my_bool reconnect = 1;

            poolSize = (size_t) pooledConn;

            for (size_t i = 0; i < poolSize; ++i)
                {
                    soci::session& sql = (*connectionPool).at(i);
                    sql.open(soci::mysql, connStr);

                    (*connectionPool).at(i) << "SET SESSION tx_isolation = 'READ-COMMITTED'";

                    soci::mysql_session_backend* be = static_cast<soci::mysql_session_backend*>(sql.get_backend());
                    mysql_options(static_cast<MYSQL*>(be->conn_), MYSQL_OPT_RECONNECT, &reconnect);
                }
        }
    catch (std::exception& e)
        {
            if(connectionPool)
                {
                    delete connectionPool;
                    connectionPool = NULL;
                }
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            if(connectionPool)
                {
                    delete connectionPool;
                    connectionPool = NULL;
                }
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




TransferJobs* MySqlAPI::getTransferJob(std::string jobId, bool archive)
{
    soci::session sql(*connectionPool);

    std::string query;
    if (archive)
        {
            query = "SELECT t_job_backup.vo_name, t_job_backup.user_dn "
                    "FROM t_job_backup WHERE t_job_backup.job_id = :jobId";
        }
    else
        {
            query = "SELECT t_job.vo_name, t_job.user_dn "
                    "FROM t_job WHERE t_job.job_id = :jobId";
        }

    TransferJobs* job = NULL;
    try
        {
            job = new TransferJobs();

            sql << query,
                soci::use(jobId),
                soci::into(job->VO_NAME), soci::into(job->USER_DN);

            if (!sql.got_data())
                {
                    delete job;
                    job = NULL;
                }
        }
    catch (std::exception& e)
        {
            if(job)
                delete job;
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            if(job)
                delete job;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return job;
}


std::map<std::string, double> MySqlAPI::getActivityShareConf(soci::session& sql, std::string vo)
{

    std::map<std::string, double> ret;

    soci::indicator isNull = soci::i_ok;
    std::string activity_share_str;
    try
        {

            sql <<
                " SELECT activity_share "
                " FROM t_activity_share_config "
                " WHERE vo = :vo "
                "	AND active = 'on'",
                soci::use(vo),
                soci::into(activity_share_str, isNull)
                ;

            if (isNull == soci::i_null || activity_share_str.empty()) return ret;

            // remove the opening '[' and closing ']'
            activity_share_str = activity_share_str.substr(1, activity_share_str.size() - 2);

            // iterate over activity shares
            boost::char_separator<char> sep(",");
            boost::tokenizer< boost::char_separator<char> > tokens(activity_share_str, sep);
            boost::tokenizer< boost::char_separator<char> >::iterator it;

            static const boost::regex re("^\\s*\\{\\s*\"([a-zA-Z0-9\\.-]+)\"\\s*:\\s*(0\\.\\d+)\\s*\\}\\s*$");
            static const int ACTIVITY_NAME = 1;
            static const int ACTIVITY_SHARE = 2;

            for (it = tokens.begin(); it != tokens.end(); it++)
                {
                    // parse single activity share
                    std::string str = *it;

                    boost::smatch what;
                    boost::regex_match(str, what, re, boost::match_extra);

                    ret[what[ACTIVITY_NAME]] = boost::lexical_cast<double>(what[ACTIVITY_SHARE]);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return ret;
}

std::vector<std::string> MySqlAPI::getAllActivityShareConf()
{
    soci::session sql(*connectionPool);

    std::vector<std::string> ret;

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT vo "
                                             " FROM t_activity_share_config "
                                         );

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); it++)
                {
                    ret.push_back(it->get<std::string>("vo"));
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

std::map<std::string, long long> MySqlAPI::getActivitiesInQueue(soci::session& sql, std::string src, std::string dst, std::string vo)
{
    std::map<std::string, long long> ret;

    time_t now = time(NULL);
    struct tm tTime;
    gmtime_r(&now, &tTime);

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT activity, COUNT(DISTINCT f.job_id, f.file_index) AS count "
                                             " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) WHERE "
                                             "  j.vo_name = f.vo_name AND f.file_state = 'SUBMITTED' AND "
                                             "	f.source_se = :source AND f.dest_se = :dest AND "
                                             "	f.vo_name = :vo_name AND "
                                             "	f.wait_timestamp IS NULL AND "
                                             "	(f.retry_timestamp is NULL OR f.retry_timestamp < :tTime) AND "
                                             "	(f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) AND "
                                             "  j.job_state in ('ACTIVE','READY','SUBMITTED') AND "
                                             "  (j.reuse_job = 'N' OR j.reuse_job IS NULL) "
                                             " GROUP BY activity ",
                                             soci::use(src),
                                             soci::use(dst),
                                             soci::use(vo),
                                             soci::use(tTime),
                                             soci::use(hashSegment.start),
                                             soci::use(hashSegment.end)
                                         );

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); it++)
                {
                    if (it->get_indicator("activity") == soci::i_null)
                        {
                            ret["default"] = it->get<long long>("count");
                        }
                    else
                        {
                            std::string activityShare = it->get<std::string>("activity");
                            long long nFiles = it->get<long long>("count");
                            ret[activityShare.empty() ? "default" : activityShare] = nFiles;
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }

    return ret;
}

std::map<std::string, int> MySqlAPI::getFilesNumPerActivity(soci::session& sql, std::string src, std::string dst, std::string vo, int filesNum)
{
    std::map<std::string, int> activityFilesNum;

    try
        {

            // get activity shares configuration for given VO
            std::map<std::string, double> activityShares = getActivityShareConf(sql, vo);

            // if there is no configuration no assigment can be made
            if (activityShares.empty()) return activityFilesNum;

            // get the activities in the queue
            std::map<std::string, long long> activitiesInQueue = getActivitiesInQueue(sql, src, dst, vo);

            // sum of all activity shares in the queue (needed for normalization)
            double sum = 0;

            std::map<std::string, long long>::iterator it;
            for (it = activitiesInQueue.begin(); it != activitiesInQueue.end(); it++)
                {
                    sum += activityShares[it->first];
                }

            // assign slots to activities
            for (int i = 0; i < filesNum; i++)
                {
                    // if sum <= 0 there is nothing to assign
                    if (sum <= 0) break;
                    // a random number from (0, 1)
                    double r = ((double) rand() / (RAND_MAX));
                    // interval corresponding to given activity
                    double interval = 0;

                    for (it = activitiesInQueue.begin(); it != activitiesInQueue.end(); it++)
                        {
                            // if there are no more files for this activity continue
                            if (it->second <= 0) continue;
                            // calculate the interval
                            interval += activityShares[it->first] / sum;
                            // if the slot has been assigned to the given activity ...
                            if (r < interval)
                                {
                                    ++activityFilesNum[it->first];
                                    --it->second;
                                    // if there are no more files for the given ativity remove it from the sum
                                    if (it->second == 0) sum -= activityShares[it->first];
                                    break;
                                }
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return activityFilesNum;
}


void MySqlAPI::getByJobId(std::map< std::string, std::list<TransferFiles*> >& files)
{
    soci::session sql(*connectionPool);

    time_t now = time(NULL);
    struct tm tTime;
    std::vector< boost::tuple<std::string, std::string, std::string> > distinct;
    distinct.reserve(1500); //approximation
    int count = 0;
    bool manualConfigExists = false;
    int defaultFilesNum = 10;
    int filesNum = defaultFilesNum;
    int limit = 0;
    int maxActive = 0;
    soci::indicator isNull = soci::i_ok;
    std::string vo_name;
    std::string source_se;
    std::string dest_se;

    try
        {
            soci::rowset<std::string> rs1 = (
                                                sql.prepare <<
                                                " SELECT DISTINCT vo_name FROM t_job WHERE job_finished is null ");

            for (soci::rowset<std::string>::const_iterator i1 = rs1.begin(); i1 != rs1.end(); ++i1)
                {
                    vo_name = (*i1);

                    soci::rowset<soci::row> rs2 = (sql.prepare <<
                                                   " SELECT DISTINCT source_se, dest_se "
                                                   " FROM t_file "
                                                   " WHERE "
                                                   "      file_state = 'SUBMITTED' AND "
                                                   "      (hashed_id >= :hStart AND hashed_id <= :hEnd) AND vo_name = :vo_name ",
                                                   soci::use(hashSegment.start), soci::use(hashSegment.end),soci::use(vo_name)
                                                  );

                    for (soci::rowset<soci::row>::const_iterator i2 = rs2.begin(); i2 != rs2.end(); ++i2)
                        {
                            soci::row const& r2 = *i2;
                            source_se = r2.get<std::string>("source_se","");
                            dest_se = r2.get<std::string>("dest_se","");

                            if(source_se.length()>0 && dest_se.length()>0 && vo_name.length()>0)
                                {
                                    distinct.push_back(
                                        boost::tuple< std::string, std::string, std::string>(
                                            source_se,
                                            dest_se,
                                            vo_name
                                        )
                                    );

                                    long long int linkExists = 0;
                                    sql << "select count(*) from t_optimize_active where source_se=:source_se and dest_se=:dest_se",
                                        soci::use(source_se),
                                        soci::use(dest_se),
                                        soci::into(linkExists);
                                    if(linkExists == 0) //for some reason does not exist, add it
                                        sql << "INSERT INTO t_optimize_active (source_se, dest_se) VALUES (:source_se, :dest_se) ON DUPLICATE KEY UPDATE source_se=:source_se, dest_se=:dest_se",
                                            soci::use(source_se), soci::use(dest_se),soci::use(source_se), soci::use(dest_se);
                                }
                        }
                }

            if(distinct.empty())
                return;

            long long hostCount = 0;
            sql <<
                " SELECT COUNT(hostname) "
                " FROM t_hosts "
                " WHERE beat >= DATE_SUB(UTC_TIMESTAMP(), interval 2 minute)",
                soci::into(hostCount)
                ;

            if(hostCount < 1)
                hostCount = 1;

            // Iterate through pairs, getting jobs IF the VO has not run out of credits
            // AND there are pending file transfers within the job
            boost::tuple<std::string, std::string, std::string> triplet;

            soci::statement stmt1 = (sql.prepare <<
                                     "SELECT COUNT(*) FROM t_link_config WHERE (source = :source OR source = '*') AND (destination = :dest OR destination = '*')",
                                     soci::use(boost::get<0>(triplet)), soci::use(boost::get<1>(triplet)), soci::into(count)
                                    );
            soci::statement stmt2 = (sql.prepare <<
                                     "SELECT COUNT(*) FROM t_group_members WHERE (member=:source OR member=:dest)",
                                     soci::use(boost::get<0>(triplet)), soci::use(boost::get<1>(triplet)), soci::into(count)
                                    );
            soci::statement stmt3 = (sql.prepare <<
                                     " select count(*) from t_file where source_se=:source_se and dest_se=:dest_se and file_state in ('READY','ACTIVE') ",
                                     soci::use(boost::get<0>(triplet)),
                                     soci::use(boost::get<1>(triplet)),
                                     soci::into(limit)
                                    );

            soci::statement stmt4 = (sql.prepare <<
                                     "select active from t_optimize_active where source_se=:source_se and dest_se=:dest_se",
                                     soci::use(boost::get<0>(triplet)),
                                     soci::use(boost::get<1>(triplet)),
                                     soci::into(maxActive, isNull)
                                    );


            std::vector< boost::tuple<std::string, std::string, std::string> >::iterator it;
            for (it = distinct.begin(); it != distinct.end(); ++it)
                {
                    triplet = *it;
                    count = 0;
                    manualConfigExists = false;
                    filesNum = defaultFilesNum;

                    //1st check if manual config exists
                    stmt1.execute(true);
                    if(count > 0)
                        manualConfigExists = true;

                    //if 1st check is false, check 2nd time for manual config
                    if(!manualConfigExists)
                        {
                            stmt2.execute(true);
                            if(count > 0)
                                manualConfigExists = true;
                        }

                    //both previously check returned false, you optimizer
                    if(!manualConfigExists)
                        {
                            limit = 0;
                            maxActive = 0;
                            isNull = soci::i_ok;

                            stmt3.execute(true);

                            stmt4.execute(true);

                            if (isNull != soci::i_null && maxActive > 0)
                                {
                                    filesNum = (maxActive - limit);
                                    if(filesNum <=0 )
                                        {
                                            continue;
                                        }
                                    else
                                        {
                                            filesNum /= int(hostCount);
                                            if(filesNum < 2)
                                                filesNum = 2;
                                        }
                                }
                        }
                    else
                        {
                            // round it up
                            double temp = (double) filesNum / (double)hostCount;
                            filesNum = static_cast<int>(ceil(temp));
                            // not less than 2
                            if (filesNum < 2) filesNum = 2;
                        }

                    std::map<std::string, int> activityFilesNum =
                        getFilesNumPerActivity(sql, boost::get<0>(triplet), boost::get<1>(triplet), boost::get<2>(triplet), filesNum);

                    gmtime_r(&now, &tTime);

                    if (activityFilesNum.empty())
                        {

                            soci::rowset<TransferFiles> rs = (
                                                                 sql.prepare <<
                                                                 " SELECT SQL_NO_CACHE "
                                                                 "       f.file_state, f.source_surl, f.dest_surl, f.job_id, j.vo_name, "
                                                                 "       f.file_id, j.overwrite_flag, j.user_dn, j.cred_id, "
                                                                 "       f.checksum, j.checksum_method, j.source_space_token, "
                                                                 "       j.space_token, j.copy_pin_lifetime, j.bring_online, "
                                                                 "       f.user_filesize, f.file_metadata, j.job_metadata, f.file_index, f.bringonline_token, "
                                                                 "       f.source_se, f.dest_se, f.selection_strategy, j.internal_job_params "
                                                                 " FROM t_file f LEFT JOIN t_job j ON (f.job_id = j.job_id) WHERE  "
                                                                 " j.vo_name = f.vo_name AND f.file_state = 'SUBMITTED' AND j.job_finished is NULL AND "
                                                                 "    f.source_se = :source AND f.dest_se = :dest AND "
                                                                 "    f.vo_name = :vo_name AND "
                                                                 "    f.wait_timestamp IS NULL AND "
                                                                 "    (f.retry_timestamp is NULL OR f.retry_timestamp < :tTime) AND "
                                                                 "    (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) AND "
                                                                 "    j.job_state in ('ACTIVE','READY','SUBMITTED') AND "
                                                                 "    (j.reuse_job = 'N' OR j.reuse_job IS NULL) "
                                                                 "     ORDER BY j.priority DESC, j.submit_time LIMIT :filesNum ",
                                                                 soci::use(boost::get<0>(triplet)),
                                                                 soci::use(boost::get<1>(triplet)),
                                                                 soci::use(boost::get<2>(triplet)),
                                                                 soci::use(tTime),
                                                                 soci::use(hashSegment.start), soci::use(hashSegment.end),
                                                                 soci::use(filesNum)
                                                             );

                            for (soci::rowset<TransferFiles>::const_iterator ti = rs.begin(); ti != rs.end(); ++ti)
                                {
                                    TransferFiles const& tfile = *ti;
                                    files[tfile.VO_NAME].push_back(new TransferFiles(tfile));
                                }
                        }
                    else
                        {
                            std::map<std::string, int>::iterator it_act;

                            for (it_act = activityFilesNum.begin(); it_act != activityFilesNum.end(); ++it_act)
                                {
                                    if (it_act->second == 0) continue;

                                    std::string select =
                                        " SELECT "
                                        "       f.file_state, f.source_surl, f.dest_surl, f.job_id, j.vo_name, "
                                        "       f.file_id, j.overwrite_flag, j.user_dn, j.cred_id, "
                                        "       f.checksum, j.checksum_method, j.source_space_token, "
                                        "       j.space_token, j.copy_pin_lifetime, j.bring_online, "
                                        "       f.user_filesize, f.file_metadata, j.job_metadata, f.file_index, f.bringonline_token, "
                                        "       f.source_se, f.dest_se, f.selection_strategy, j.internal_job_params  "
                                        " FROM t_file f LEFT JOIN t_job j ON (f.job_id = j.job_id) WHERE "
                                        " j.vo_name = f.vo_name AND f.file_state = 'SUBMITTED' AND  "
                                        "    f.source_se = :source AND f.dest_se = :dest AND "
                                        "    f.vo_name = :vo_name AND ";
                                    select +=
                                        it_act->first == "default" ?
                                        "	  (f.activity = :activity OR f.activity = '' OR f.activity IS NULL) AND "
                                        :
                                        "	  f.activity = :activity AND ";
                                    select +=
                                        "    f.wait_timestamp IS NULL AND "
                                        "    (f.retry_timestamp is NULL OR f.retry_timestamp < :tTime) AND "
                                        "    (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) AND "
                                        "    j.job_state in ('ACTIVE','READY','SUBMITTED') AND "
                                        "    (j.reuse_job = 'N' OR j.reuse_job IS NULL)  "
                                        "    ORDER BY j.priority DESC, j.submit_time LIMIT :filesNum"
                                        ;


                                    soci::rowset<TransferFiles> rs = (
                                                                         sql.prepare <<
                                                                         select,
                                                                         soci::use(boost::get<0>(triplet)),
                                                                         soci::use(boost::get<1>(triplet)),
                                                                         soci::use(boost::get<2>(triplet)),
                                                                         soci::use(it_act->first),
                                                                         soci::use(tTime),
                                                                         soci::use(hashSegment.start), soci::use(hashSegment.end),
                                                                         soci::use(it_act->second)
                                                                     );

                                    for (soci::rowset<TransferFiles>::const_iterator ti = rs.begin(); ti != rs.end(); ++ti)
                                        {
                                            TransferFiles const& tfile = *ti;
                                            files[tfile.VO_NAME].push_back(new TransferFiles(tfile));
                                        }
                                }
                        }
                }
        }
    catch (std::exception& e)
        {
            for (std::map< std::string, std::list<TransferFiles*> >::iterator i = files.begin(); i != files.end(); ++i)
                {
                    std::list<TransferFiles*>& l = i->second;
                    for (std::list<TransferFiles*>::iterator it = l.begin(); it != l.end(); ++it)
                        {
                            if(*it)
                                delete *it;
                        }
                    l.clear();
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            for (std::map< std::string, std::list<TransferFiles*> >::iterator i = files.begin(); i != files.end(); ++i)
                {
                    std::list<TransferFiles*>& l = i->second;
                    for (std::list<TransferFiles*>::iterator it = l.begin(); it != l.end(); ++it)
                        {
                            if(*it)
                                delete *it;
                        }
                    l.clear();
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}




void MySqlAPI::setFilesToNotUsed(std::string jobId, int fileIndex, std::vector<int>& files)
{
    soci::session sql(*connectionPool);

    try
        {

            // first really check if it is a multi-source/destination submission
            // count the alternative replicas, if there is more than one it makes sense to set the NOT_USED state

            int count = 0;

            soci::statement stmt1 = (
                                        sql.prepare << "SELECT COUNT(*) "
                                        "FROM t_file "
                                        "WHERE job_id = :jobId AND file_index = :fileIndex",
                                        soci::use(jobId),
                                        soci::use(fileIndex),
                                        soci::into(count));
            stmt1.execute(true);

            if (count < 2) return;

            sql.begin();
            soci::statement stmt(sql);

            stmt.exchange(soci::use(jobId, "jobId"));
            stmt.exchange(soci::use(fileIndex, "fileIndex"));
            stmt.alloc();
            stmt.prepare(
                "UPDATE t_file "
                "SET file_state = 'NOT_USED' "
                "WHERE "
                "	job_id = :jobId AND "
                "	file_index = :fileIndex "
                "	AND file_state = 'SUBMITTED' "
            );
            stmt.define_and_bind();
            stmt.execute(true);
            long long  affected_rows = stmt.get_affected_rows();
            sql.commit();

            if (affected_rows > 0)
                {

                    soci::rowset<soci::row> rs = (
                                                     sql.prepare <<
                                                     " SELECT file_id "
                                                     " FROM t_file "
                                                     " WHERE job_id = :jobId "
                                                     "	AND file_index = :fileIndex "
                                                     "	AND file_state = 'NOT_USED' ",
                                                     soci::use(jobId),
                                                     soci::use(fileIndex)
                                                 );

                    soci::rowset<soci::row>::const_iterator it;
                    for (it = rs.begin(); it != rs.end(); ++it)
                        {
                            files.push_back(
                                it->get<int>("file_id")
                            );
                        }
                }


        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::useFileReplica(soci::session& sql, std::string jobId, int fileId)
{
    try
        {
            soci::indicator ind = soci::i_ok;
            int fileIndex=0;

            sql <<
                " SELECT file_index "
                " FROM t_file "
                " WHERE file_id = :fileId AND file_state = 'NOT_USED' ",
                soci::use(fileId),
                soci::into(fileIndex, ind)
                ;

            // make sure it's not NULL
            if (ind == soci::i_ok)
                {
                    sql.begin();
                    sql <<
                        " UPDATE t_file "
                        " SET file_state = 'SUBMITTED' "
                        " WHERE job_id = :jobId "
                        "	AND file_index = :fileIndex "
                        "	AND file_state = 'NOT_USED'",
                        soci::use(jobId),
                        soci::use(fileIndex);
                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

}

unsigned int MySqlAPI::updateFileStatusReuse(TransferFiles* file, const std::string status)
{
    soci::session sql(*connectionPool);

    unsigned int updated = 0;


    try
        {
            sql.begin();

            soci::statement stmt(sql);

            stmt.exchange(soci::use(status, "state"));
            stmt.exchange(soci::use(file->JOB_ID, "jobId"));
            stmt.exchange(soci::use(hostname, "hostname"));
            stmt.alloc();
            stmt.prepare("UPDATE t_file SET "
                         "    file_state = :state, start_time = UTC_TIMESTAMP(), transferHost = :hostname "
                         "WHERE job_id = :jobId AND file_state = 'SUBMITTED'");
            stmt.define_and_bind();
            stmt.execute(true);

            updated = (unsigned int) stmt.get_affected_rows();

            if (updated > 0)
                {
                    soci::statement jobStmt(sql);
                    jobStmt.exchange(soci::use(status, "state"));
                    jobStmt.exchange(soci::use(file->JOB_ID, "jobId"));
                    jobStmt.alloc();
                    jobStmt.prepare("UPDATE t_job SET "
                                    "    job_state = :state "
                                    "WHERE job_id = :jobId AND job_state = 'SUBMITTED'");
                    jobStmt.define_and_bind();
                    jobStmt.execute(true);
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return updated;
}


unsigned int MySqlAPI::updateFileStatus(TransferFiles* file, const std::string status)
{
    soci::session sql(*connectionPool);

    unsigned int updated = 0;


    try
        {
            int countSame = 0;

            soci::statement stmt1 = (
                                        sql.prepare << "select count(*) from t_file where file_state in ('READY','ACTIVE') and dest_surl=:destUrl and vo_name=:vo_name and dest_se=:dest_se ",
                                        soci::use(file->DEST_SURL),
                                        soci::use(file->VO_NAME),
                                        soci::use(file->DEST_SE),
                                        soci::into(countSame));
            stmt1.execute(true);


            if(countSame > 0)
                {
                    return 0;
                }


            sql.begin();

            soci::statement stmt(sql);

            stmt.exchange(soci::use(status, "state"));
            stmt.exchange(soci::use(file->FILE_ID, "fileId"));
            stmt.exchange(soci::use(hostname, "hostname"));
            stmt.alloc();
            stmt.prepare("UPDATE t_file SET "
                         "    file_state = :state, start_time = UTC_TIMESTAMP(), transferHost = :hostname "
                         "WHERE file_id = :fileId AND file_state = 'SUBMITTED'");
            stmt.define_and_bind();
            stmt.execute(true);

            updated = (unsigned int) stmt.get_affected_rows();
            if (updated > 0)
                {
                    soci::statement jobStmt(sql);
                    jobStmt.exchange(soci::use(status, "state"));
                    jobStmt.exchange(soci::use(file->JOB_ID, "jobId"));
                    jobStmt.alloc();
                    jobStmt.prepare("UPDATE t_job SET "
                                    "    job_state = :state "
                                    "WHERE job_id = :jobId AND job_state = 'SUBMITTED'");
                    jobStmt.define_and_bind();
                    jobStmt.execute(true);
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return updated;
}


void MySqlAPI::getByJobIdReuse(std::vector<TransferJobs*>& jobs, std::map< std::string, std::list<TransferFiles*> >& files)
{
    soci::session sql(*connectionPool);

    time_t now = time(NULL);
    struct tm tTime;
    gmtime_r(&now, &tTime);

    try
        {
            for (std::vector<TransferJobs*>::const_iterator i = jobs.begin(); i != jobs.end(); ++i)
                {
                    std::string jobId = (*i)->JOB_ID;

                    soci::rowset<TransferFiles> rs = (
                                                         sql.prepare <<
                                                         "SELECT "
                                                         "       f.file_state, f.source_surl, f.dest_surl, f.job_id, j.vo_name, "
                                                         "       f.file_id, j.overwrite_flag, j.user_dn, j.cred_id, "
                                                         "       f.checksum, j.checksum_method, j.source_space_token, "
                                                         "       j.space_token, j.copy_pin_lifetime, j.bring_online, "
                                                         "       f.user_filesize, f.file_metadata, j.job_metadata, f.file_index, f.bringonline_token, "
                                                         "       f.source_se, f.dest_se, f.selection_strategy, j.internal_job_params "
                                                         "FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                                         "WHERE f.job_id = :jobId AND"
                                                         "    f.file_state = 'SUBMITTED' AND "
                                                         "    f.job_finished IS NULL AND "
                                                         "    f.wait_timestamp IS NULL AND "
                                                         "    (f.retry_timestamp is NULL OR f.retry_timestamp < :tTime) AND "
                                                         "    (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) ",
                                                         soci::use(jobId),
                                                         soci::use(tTime),
                                                         soci::use(hashSegment.start), soci::use(hashSegment.end)
                                                     );


                    for (soci::rowset<TransferFiles>::const_iterator ti = rs.begin(); ti != rs.end(); ++ti)
                        {
                            TransferFiles const& tfile = *ti;
                            files[tfile.VO_NAME].push_back(new TransferFiles(tfile));
                        }
                }
        }
    catch (std::exception& e)
        {
            for (std::map< std::string, std::list<TransferFiles*> >::iterator i = files.begin(); i != files.end(); ++i)
                {
                    std::list<TransferFiles*>& l = i->second;
                    for (std::list<TransferFiles*>::iterator it = l.begin(); it != l.end(); ++it)
                        {
                            if(*it)
                                delete *it;
                        }
                    l.clear();
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            for (std::map< std::string, std::list<TransferFiles*> >::iterator i = files.begin(); i != files.end(); ++i)
                {
                    std::list<TransferFiles*>& l = i->second;
                    for (std::list<TransferFiles*>::iterator it = l.begin(); it != l.end(); ++it)
                        {
                            if(*it)
                                delete *it;
                        }
                    l.clear();
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




void MySqlAPI::submitPhysical(const std::string & jobId, std::list<job_element_tupple> src_dest_pair,
                              const std::string & DN, const std::string & cred,
                              const std::string & voName, const std::string & myProxyServer, const std::string & delegationID,
                              const std::string & sourceSe, const std::string & destinationSe,
                              const JobParameterHandler & params)
{
    const int         bringOnline      = params.get<int>(JobParameterHandler::BRING_ONLINE);
    const std::string checksumMethod   = params.get(JobParameterHandler::CHECKSUM_METHOD);
    const int         copyPinLifeTime  = params.get<int>(JobParameterHandler::COPY_PIN_LIFETIME);
    const std::string failNearLine     = params.get(JobParameterHandler::FAIL_NEARLINE);
    const std::string metadata         = params.get(JobParameterHandler::JOB_METADATA);
    const std::string overwrite        = params.get(JobParameterHandler::OVERWRITEFLAG);
    const std::string paramFTP         = params.get(JobParameterHandler::GRIDFTP);
    const int         retry            = params.get<int>(JobParameterHandler::RETRY);
    const int         retryDelay       = params.get<int>(JobParameterHandler::RETRY_DELAY);
    const std::string reuse            = params.get(JobParameterHandler::REUSE);
    const std::string hop              = params.get(JobParameterHandler::MULTIHOP);
    const std::string sourceSpaceToken = params.get(JobParameterHandler::SPACETOKEN_SOURCE);
    const std::string spaceToken       = params.get(JobParameterHandler::SPACETOKEN);
    const std::string nostreams		   = params.get(JobParameterHandler::NOSTREAMS);
    const std::string buffSize		   = params.get(JobParameterHandler::BUFFER_SIZE);
    const std::string timeout		   = params.get(JobParameterHandler::TIMEOUT);

    std::string reuseFlag = "N";
    if (reuse == "Y")
        reuseFlag = "Y";
    else if (hop == "Y")
        reuseFlag = "H";

    const std::string initialState = bringOnline > 0 || copyPinLifeTime > 0 ? "STAGING" : "SUBMITTED";
    const int priority = 3;
    std::string jobParams;

    // create the internal params string
    // if numbre of streams was specified ...
    if (!nostreams.empty()) jobParams = "nostreams:" + nostreams;
    // if timeout was specified ...
    if (!timeout.empty())
        {
            if (!jobParams.empty()) jobParams += ",";
            jobParams += "timeout:" + timeout;
        }
    // if buffer size was specified ...
    if (!buffSize.empty())
        {
            if (!jobParams.empty()) jobParams += ",";
            jobParams += "buffersize:" + buffSize;
        }

    soci::session sql(*connectionPool);

    //multiple insert statements
    std::ostringstream pairStmt;
    std::ostringstream pairStmtSeBlaklisted;

    //we can no longer use mysql functions, so create UTC timestamp outside
    time_t now = time(NULL);
    struct tm tTime;
    gmtime_r(&now, &tTime);

    try
        {
            sql.begin();
            soci::indicator reuseFlagIndicator = soci::i_ok;
            if (reuseFlag.empty())
                reuseFlagIndicator = soci::i_null;
            // Insert job
            soci::statement insertJob = ( sql.prepare << "INSERT INTO t_job (job_id, job_state, job_params, user_dn, user_cred, priority,       "
                                          "                   vo_name, submit_time, internal_job_params, submit_host, cred_id,   "
                                          "                   myproxy_server, space_token, overwrite_flag, source_space_token,   "
                                          "                   copy_pin_lifetime, fail_nearline, checksum_method, "
                                          "                   reuse_job, bring_online, retry, retry_delay, job_metadata,		  "
                                          "			       source_se, dest_se)         										  "
                                          "VALUES (:jobId, :jobState, :jobParams, :userDn, :userCred, :priority,                 "
                                          "        :voName, UTC_TIMESTAMP(), :internalParams, :submitHost, :credId,              "
                                          "        :myproxyServer, :spaceToken, :overwriteFlag, :sourceSpaceToken,               "
                                          "        :copyPinLifetime, :failNearline, :checksumMethod,             "
                                          "        :reuseJob, :bring_online, :retry, :retryDelay, :job_metadata,				  "
                                          "		:sourceSe, :destSe)															  ",
                                          soci::use(jobId), soci::use(initialState), soci::use(paramFTP), soci::use(DN), soci::use(cred), soci::use(priority),
                                          soci::use(voName), soci::use(jobParams), soci::use(hostname), soci::use(delegationID),
                                          soci::use(myProxyServer), soci::use(spaceToken), soci::use(overwrite), soci::use(sourceSpaceToken),
                                          soci::use(copyPinLifeTime), soci::use(failNearLine), soci::use(checksumMethod),
                                          soci::use(reuseFlag, reuseFlagIndicator), soci::use(bringOnline),
                                          soci::use(retry), soci::use(retryDelay), soci::use(metadata),
                                          soci::use(sourceSe), soci::use(destinationSe));

            insertJob.execute(true);

            std::string sourceSurl, destSurl, checksum, metadata, selectionStrategy, sourceSe, destSe, activity;
            double filesize = 0.0;
            unsigned hashedId = 0;
            int fileIndex = 0, timeout = 0;
            typedef std::pair<std::string, std::string> Key;
            typedef std::map< Key , int> Mapa;
            Mapa mapa;

            //create the insertion statements here and populate values inside the loop
            pairStmt << std::fixed << "INSERT INTO t_file (vo_name, job_id, file_state, source_surl, dest_surl,checksum, user_filesize, file_metadata, selection_strategy, file_index, source_se, dest_se, activity, hashed_id) VALUES ";

            pairStmtSeBlaklisted << std::fixed << "INSERT INTO t_file (vo_name, job_id, file_state, source_surl, dest_surl, checksum, user_filesize, file_metadata, selection_strategy, file_index, source_se, dest_se, wait_timestamp, wait_timeout, activity, hashed_id) VALUES ";

            // When reuse is enabled, we use the same random number for the whole job
            // This guarantees that the whole set belong to the same machine, but keeping
            // the load balance between hosts
            if (reuseFlag != "N")
                hashedId = getHashedId();

            std::list<job_element_tupple>::const_iterator iter;
            for (iter = src_dest_pair.begin(); iter != src_dest_pair.end(); ++iter)
                {
                    sourceSurl = iter->source;
                    destSurl   = iter->destination;
                    checksum   = iter->checksum;
                    filesize   = iter->filesize;
                    metadata   = iter->metadata;
                    selectionStrategy = iter->selectionStrategy;
                    fileIndex = iter->fileIndex;
                    sourceSe = iter->source_se;
                    destSe = iter->dest_se;
                    activity = iter->activity;

                    // No reuse, one random per file
                    if (reuseFlag == "N")
                        hashedId = getHashedId();

                    //get distinct source_se / dest_se
                    Key p1 (sourceSe, destSe);
                    mapa.insert(std::make_pair(p1, 0));

                    if (iter->wait_timeout.is_initialized())
                        {
                            timeout = *iter->wait_timeout;
                            pairStmtSeBlaklisted << "(";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << voName;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << jobId;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << initialState;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << sourceSurl;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << destSurl;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << checksum;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << filesize;
                            pairStmtSeBlaklisted << ",";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << metadata;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << selectionStrategy;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << fileIndex;
                            pairStmtSeBlaklisted << ",";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << sourceSe;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << destSe;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << asctime(&tTime);
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << timeout;
                            pairStmtSeBlaklisted << ",";
                            pairStmtSeBlaklisted << "'";
                            pairStmtSeBlaklisted << activity;
                            pairStmtSeBlaklisted << "',";
                            pairStmtSeBlaklisted << hashedId;
                            pairStmtSeBlaklisted << "),";
                        }
                    else
                        {
                            pairStmt << "(";
                            pairStmt << "'";
                            pairStmt << voName;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << jobId;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << initialState;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << sourceSurl;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << destSurl;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << checksum;
                            pairStmt << "',";
                            pairStmt << filesize;
                            pairStmt << ",";
                            pairStmt << "'";
                            pairStmt << metadata;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << selectionStrategy;
                            pairStmt << "',";
                            pairStmt << fileIndex;
                            pairStmt << ",";
                            pairStmt << "'";
                            pairStmt << sourceSe;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << destSe;
                            pairStmt << "',";
                            pairStmt << "'";
                            pairStmt << activity;
                            pairStmt << "',";
                            pairStmt << hashedId;
                            pairStmt << "),";
                        }
                }

            if(timeout == 0)
                {
                    std::string queryStr = pairStmt.str();
                    sql << queryStr.substr(0, queryStr.length() - 1);
                }
            else
                {
                    std::string queryStr = pairStmtSeBlaklisted.str();
                    sql << queryStr.substr(0, queryStr.length() - 1);
                }


            std::map<Key , int>::const_iterator itr;
            for(itr = mapa.begin(); itr != mapa.end(); ++itr)
                {
                    Key p1 = (*itr).first;
                    std::string source_se = p1.first;
                    std::string dest_se = p1.second;
                    sql << "INSERT INTO t_optimize_active (source_se, dest_se) VALUES (:source_se, :dest_se) ON DUPLICATE KEY UPDATE source_se=:source_se, dest_se=:dest_se",
                        soci::use(source_se), soci::use(dest_se),soci::use(source_se), soci::use(dest_se);
                }

            sql.commit();
            pairStmt.str(std::string());
            pairStmt.clear();
            pairStmtSeBlaklisted.str(std::string());
            pairStmtSeBlaklisted.clear();
        }
    catch (std::exception& e)
        {
            pairStmt.str(std::string());
            pairStmt.clear();
            pairStmtSeBlaklisted.str(std::string());
            pairStmtSeBlaklisted.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            pairStmt.str(std::string());
            pairStmt.clear();
            pairStmtSeBlaklisted.str(std::string());
            pairStmtSeBlaklisted.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




void MySqlAPI::getTransferJobStatus(std::string requestID, bool archive, std::vector<JobStatus*>& jobs)
{
    soci::session sql(*connectionPool);

    std::string fileCountQuery;
    std::string statusQuery;

    if (archive)
        {
            fileCountQuery = "SELECT COUNT(DISTINCT file_index) FROM t_file_backup WHERE t_file_backup.job_id = :jobId";
            statusQuery = "SELECT t_job_backup.job_id, t_job_backup.job_state, t_file_backup.file_state, "
                          "    t_job_backup.user_dn, t_job_backup.reason, t_job_backup.submit_time, t_job_backup.priority, "
                          "    t_job_backup.vo_name, t_file_backup.file_index "
                          "FROM t_job_backup, t_file_backup "
                          "WHERE t_file_backup.job_id = :jobId and t_file_backup.job_id= t_job_backup.job_id";
        }
    else
        {
            fileCountQuery = "SELECT COUNT(DISTINCT file_index) FROM t_file WHERE t_file.job_id = :jobId";
            statusQuery = "SELECT t_job.job_id, t_job.job_state, t_file.file_state, "
                          "    t_job.user_dn, t_job.reason, t_job.submit_time, t_job.priority, "
                          "    t_job.vo_name, t_file.file_index "
                          "FROM t_job, t_file "
                          "WHERE t_file.job_id = :jobId and t_file.job_id=t_job.job_id";
        }

    try
        {
            long long numFiles = 0;
            sql << fileCountQuery, soci::use(requestID), soci::into(numFiles);

            soci::rowset<JobStatus> rs = (
                                             sql.prepare << statusQuery,
                                             soci::use(requestID, "jobId"));

            for (soci::rowset<JobStatus>::iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    JobStatus& job = *i;
                    job.numFiles = numFiles;
                    jobs.push_back(new JobStatus(job));
                }
        }
    catch (std::exception& e)
        {
            std::vector< JobStatus* >::iterator it;
            for (it = jobs.begin(); it != jobs.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            std::vector< JobStatus* >::iterator it;
            for (it = jobs.begin(); it != jobs.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



/*
 * Return a list of jobs based on the status requested
 * std::vector<JobStatus*> jobs: the caller will deallocate memory JobStatus instances and clear the vector
 * std::vector<std::string> inGivenStates: order doesn't really matter, more than one states supported
 */
void MySqlAPI::listRequests(std::vector<JobStatus*>& jobs, std::vector<std::string>& inGivenStates, std::string restrictToClientDN, std::string forDN, std::string VOname)
{
    soci::session sql(*connectionPool);

    try
        {
            std::ostringstream query;
            soci::statement stmt(sql);
            bool searchForCanceling = false;


            query << "SELECT DISTINCT job_id, job_state, reason, submit_time, user_dn, "
                  "                 vo_name, priority, cancel_job, "
                  "                 (SELECT COUNT(DISTINCT t_file.file_index) FROM t_file WHERE t_file.job_id = t_job.job_id) as numFiles "
                  "FROM t_job ";

            //joins
            if (!restrictToClientDN.empty())
                {
                    query << "LEFT OUTER JOIN t_vo_acl ON t_vo_acl.vo_name = t_job.vo_name ";
                }

            //gain the benefit from the statement pooling
            std::sort(inGivenStates.begin(), inGivenStates.end());

            if (inGivenStates.size() > 0)
                {
                    std::vector<std::string>::const_iterator i;
                    i = std::find_if(inGivenStates.begin(), inGivenStates.end(),
                                     std::bind2nd(std::equal_to<std::string>(), std::string("CANCELED")));
                    searchForCanceling = (i != inGivenStates.end());

                    std::string jobStatusesIn = "'" + inGivenStates[0] + "'";
                    for (unsigned i = 1; i < inGivenStates.size(); ++i)
                        {
                            jobStatusesIn += (",'" + inGivenStates[i] + "'");
                        }
                    query << "WHERE job_state IN (" << jobStatusesIn << ") ";
                }
            else
                {
                    query << "WHERE 1 ";
                }

            if (!restrictToClientDN.empty())
                {
                    query << " AND (t_job.user_dn = :clientDn OR t_vo_acl.principal = :clientDn) ";
                    stmt.exchange(soci::use(restrictToClientDN, "clientDn"));
                }

            if (!VOname.empty())
                {
                    query << " AND vo_name = :vo ";
                    stmt.exchange(soci::use(VOname, "vo"));
                }

            if (!forDN.empty())
                {
                    query << " AND user_dn = :userDn ";
                    stmt.exchange(soci::use(forDN, "userDn"));
                }

            if (searchForCanceling)
                {
                    query << " AND cancel_job = 'Y' ";
                }

            JobStatus job;
            stmt.exchange(soci::into(job));
            stmt.alloc();
            std::string test = query.str();
            stmt.prepare(query.str());
            stmt.define_and_bind();

            if (stmt.execute(true))
                {
                    do
                        {
                            jobs.push_back(new JobStatus(job));
                        }
                    while (stmt.fetch());
                }

        }
    catch (std::exception& e)
        {
            std::vector< JobStatus* >::iterator it;
            for (it = jobs.begin(); it != jobs.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            std::vector< JobStatus* >::iterator it;
            for (it = jobs.begin(); it != jobs.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::getTransferFileStatus(std::string requestID, bool archive,
                                     unsigned offset, unsigned limit, std::vector<FileTransferStatus*>& files)
{
    soci::session sql(*connectionPool);

    try
        {
            std::string query;

            if (archive)
                {
                    query = "SELECT t_file_backup.file_id, t_file_backup.source_surl, t_file_backup.dest_surl, t_file_backup.file_state, "
                            "       t_file_backup.reason, t_file_backup.start_time, t_file_backup.finish_time, t_file_backup.retry, t_file_backup.tx_duration "
                            "FROM t_file_backup WHERE t_file_backup.job_id = :jobId ";
                }
            else
                {
                    query = "SELECT t_file.file_id, t_file.source_surl, t_file.dest_surl, t_file.file_state, "
                            "       t_file.reason, t_file.start_time, t_file.finish_time, t_file.retry, t_file.tx_duration "
                            "FROM t_file WHERE t_file.job_id = :jobId ";
                }

            if (limit)
                query += " LIMIT :offset,:limit";
            else
                query += " LIMIT :offset,18446744073709551615";



            FileTransferStatus transfer;
            soci::statement stmt(sql);
            stmt.exchange(soci::into(transfer));
            stmt.exchange(soci::use(requestID, "jobId"));
            stmt.exchange(soci::use(offset, "offset"));
            if (limit)
                stmt.exchange(soci::use(limit, "limit"));

            stmt.alloc();
            stmt.prepare(query);
            stmt.define_and_bind();

            if (stmt.execute(true))
                {
                    do
                        {
                            files.push_back(new FileTransferStatus(transfer));
                        }
                    while (stmt.fetch());
                }
        }
    catch (std::exception& e)
        {
            std::vector< FileTransferStatus* >::iterator it;
            for (it = files.begin(); it != files.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            std::vector< FileTransferStatus* >::iterator it;
            for (it = files.begin(); it != files.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            files.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::getSe(Se* &se, std::string seName)
{
    soci::session sql(*connectionPool);
    se = NULL;

    try
        {
            se = new Se();
            sql << "SELECT * FROM t_se WHERE name = :name",
                soci::use(seName), soci::into(*se);

            if (!sql.got_data())
                {
                    delete se;
                    se = NULL;
                }

        }
    catch (std::exception& e)
        {
            if(se)
                delete se;
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            if(se)
                delete se;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::addSe(std::string ENDPOINT, std::string SE_TYPE, std::string SITE, std::string NAME, std::string STATE, std::string VERSION, std::string HOST,
                     std::string SE_TRANSFER_TYPE, std::string SE_TRANSFER_PROTOCOL, std::string SE_CONTROL_PROTOCOL, std::string GOCDB_ID)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "INSERT INTO t_se (endpoint, se_type, site, name, state, version, host, se_transfer_type, "
                "                  se_transfer_protocol, se_control_protocol, gocdb_id) VALUES "
                "                 (:endpoint, :seType, :site, :name, :state, :version, :host, :seTransferType, "
                "                  :seTransferProtocol, :seControlProtocol, :gocdbId)",
                soci::use(ENDPOINT), soci::use(SE_TYPE), soci::use(SITE), soci::use(NAME), soci::use(STATE), soci::use(VERSION),
                soci::use(HOST), soci::use(SE_TRANSFER_TYPE), soci::use(SE_TRANSFER_PROTOCOL), soci::use(SE_CONTROL_PROTOCOL),
                soci::use(GOCDB_ID);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



void MySqlAPI::updateSe(std::string ENDPOINT, std::string SE_TYPE, std::string SITE, std::string NAME, std::string STATE, std::string VERSION, std::string HOST,
                        std::string SE_TRANSFER_TYPE, std::string SE_TRANSFER_PROTOCOL, std::string SE_CONTROL_PROTOCOL, std::string GOCDB_ID)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            std::ostringstream query;
            soci::statement stmt(sql);

            query << "UPDATE t_se SET ";

            if (ENDPOINT.length() > 0)
                {
                    query << "ENDPOINT=:endpoint,";
                    stmt.exchange(soci::use(ENDPOINT, "endpoint"));
                }

            if (SE_TYPE.length() > 0)
                {
                    query << " SE_TYPE = :seType,";
                    stmt.exchange(soci::use(SE_TYPE, "seType"));
                }

            if (SITE.length() > 0)
                {
                    query << " SITE = :site,";
                    stmt.exchange(soci::use(SITE, "site"));
                }

            if (STATE.length() > 0)
                {
                    query << " STATE = :state,";
                    stmt.exchange(soci::use(STATE, "state"));
                }

            if (VERSION.length() > 0)
                {
                    query << " VERSION = :version,";
                    stmt.exchange(soci::use(VERSION, "version"));
                }

            if (HOST.length() > 0)
                {
                    query << " HOST = :host,";
                    stmt.exchange(soci::use(HOST, "host"));
                }

            if (SE_TRANSFER_TYPE.length() > 0)
                {
                    query << " SE_TRANSFER_TYPE = :transferType,";
                    stmt.exchange(soci::use(SE_TRANSFER_TYPE, "transferType"));
                }

            if (SE_TRANSFER_PROTOCOL.length() > 0)
                {
                    query << " SE_TRANSFER_PROTOCOL = :transferProtocol,";
                    stmt.exchange(soci::use(SE_TRANSFER_PROTOCOL, "transferProtocol"));
                }

            if (SE_CONTROL_PROTOCOL.length() > 0)
                {
                    query << " SE_CONTROL_PROTOCOL = :controlProtocol,";
                    stmt.exchange(soci::use(SE_CONTROL_PROTOCOL, "controlProtocol"));
                }

            if (GOCDB_ID.length() > 0)
                {
                    query << " GOCDB_ID = :gocdbId,";
                    stmt.exchange(soci::use(GOCDB_ID, "gocdbId"));
                }

            // There is always a comma at the end, so truncate
            std::string queryStr = query.str();
            query.str(std::string());

            query << queryStr.substr(0, queryStr.length() - 1);
            query << " WHERE name = :name";
            stmt.exchange(soci::use(NAME, "name"));

            stmt.alloc();
            stmt.prepare(query.str());
            stmt.define_and_bind();
            stmt.execute(true);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}


bool MySqlAPI::updateFileTransferStatus(double throughputIn, std::string job_id, int file_id, std::string transfer_status, std::string transfer_message,
                                        int process_id, double filesize, double duration, bool retry)
{

    soci::session sql(*connectionPool);
    try
        {
            updateFileTransferStatusInternal(sql, throughputIn, job_id, file_id, transfer_status, transfer_message, process_id, filesize, duration, retry);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return true;
}


bool MySqlAPI::updateFileTransferStatusInternal(soci::session& sql, double throughputIn, std::string job_id, int file_id, std::string transfer_status, std::string transfer_message,
        int process_id, double filesize, double duration, bool retry)
{
    bool ok = true;

    try
        {
            double throughput = 0.0;

            bool staging = false;

            int current_failures = retry;

            time_t now = time(NULL);
            struct tm tTime;
            gmtime_r(&now, &tTime);


            if(file_id == 0 && transfer_status == "FAILED")
                sql <<  "select file_id from t_file where pid=:pid and job_finished is NULL",soci::use(process_id), soci::into(file_id);

            if(job_id.empty() && transfer_status == "FAILED")
                sql <<  "select job_id from t_file where pid=:pid and job_finished is NULL",soci::use(process_id), soci::into(job_id);

            // query for the file state in DB
            soci::rowset<soci::row> rs = (
                                             sql.prepare << "SELECT file_state FROM t_file WHERE file_id=:fileId and job_id=:jobId",
                                             soci::use(file_id),
                                             soci::use(job_id)
                                         );


            // check if the state is STAGING, there should be just one row
            std::string st;
            soci::rowset<soci::row>::const_iterator it = rs.begin();
            if (it != rs.end())
                {
                    soci::row const& r = *it;
                    std::string st = r.get<std::string>("file_state");
                    staging = (st == "STAGING");
                }

            if(st == "ACTIVE" && (transfer_status != "FAILED" ||  transfer_status != "FINISHED" ||  transfer_status != "CANCELED"))
                return true;


            sql.begin();

            soci::statement stmt(sql);
            std::ostringstream query;

            query << "UPDATE t_file SET "
                  "    file_state = :state, reason = :reason";
            stmt.exchange(soci::use(transfer_status, "state"));
            stmt.exchange(soci::use(transfer_message, "reason"));

            if (transfer_status == "FINISHED" || transfer_status == "FAILED" || transfer_status == "CANCELED")
                {
                    query << ", FINISH_TIME = :time1";
                    query << ", JOB_FINISHED = :time1";
                    stmt.exchange(soci::use(tTime, "time1"));
                }
            if (transfer_status == "ACTIVE")
                {
                    query << ", START_TIME = :time1";
                    stmt.exchange(soci::use(tTime, "time1"));
                }

            if (transfer_status == "ACTIVE" || transfer_status == "READY")
                {
                    query << ", transferHost = :hostname";
                    stmt.exchange(soci::use(hostname, "hostname"));
                }

            if (transfer_status == "FINISHED")
                {
                    query << ", transferred = :filesize";
                    stmt.exchange(soci::use(filesize, "filesize"));
                }

            if (transfer_status == "FAILED" || transfer_status == "CANCELED")
                {
                    query << ", transferred = :transferred";
                    stmt.exchange(soci::use(0, "transferred"));
                }


            if (transfer_status == "STAGING")
                {
                    if (staging)
                        {
                            query << ", STAGING_FINISHED = :time1";
                            stmt.exchange(soci::use(tTime, "time1"));
                        }
                    else
                        {
                            query << ", STAGING_START = :time1";
                            stmt.exchange(soci::use(tTime, "time1"));
                        }
                }

            if (filesize > 0 && duration > 0 && transfer_status == "FINISHED")
                {
                    if(throughputIn != 0.0)
                        {
                            throughput = convertKbToMb(throughputIn);
                        }
                    else
                        {
                            throughput = convertBtoM(filesize, duration);
                        }
                }
            else if (filesize > 0 && duration <= 0 && transfer_status == "FINISHED")
                {
                    if(throughputIn != 0.0)
                        {
                            throughput = convertKbToMb(throughputIn);
                        }
                    else
                        {
                            throughput = convertBtoM(filesize, 1);
                        }
                }
            else
                {
                    throughput = 0.0;
                }

            query << "   , pid = :pid, filesize = :filesize, tx_duration = :duration, throughput = :throughput, current_failures = :current_failures "
                  "WHERE file_id = :fileId AND file_state NOT IN ('FAILED', 'FINISHED', 'CANCELED')";
            stmt.exchange(soci::use(process_id, "pid"));
            stmt.exchange(soci::use(filesize, "filesize"));
            stmt.exchange(soci::use(duration, "duration"));
            stmt.exchange(soci::use(throughput, "throughput"));
            stmt.exchange(soci::use(current_failures, "current_failures"));
            stmt.exchange(soci::use(file_id, "fileId"));
            stmt.alloc();
            stmt.prepare(query.str());
            stmt.define_and_bind();
            stmt.execute(true);

            sql.commit();

            if(transfer_status == "FAILED")
                useFileReplica(sql, job_id, file_id);

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
    return ok;
}


bool MySqlAPI::updateJobTransferStatus(std::string job_id, const std::string status, int pid)
{

    soci::session sql(*connectionPool);

    try
        {
            updateJobTransferStatusInternal(sql, job_id, status, pid);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return true;
}



bool MySqlAPI::updateJobTransferStatusInternal(soci::session& sql, std::string job_id, const std::string status, int pid)
{
    bool ok = true;

    try
        {
            int numberOfFilesInJob = 0;
            int numberOfFilesCanceled = 0;
            int numberOfFilesFinished = 0;
            int numberOfFilesFailed = 0;

            int numberOfFilesNotCanceled = 0;
            int numberOfFilesNotCanceledNorFailed = 0;

            std::string currentState("");
            std::string reuseFlag;
            soci::indicator isNull = soci::i_ok;

            if(job_id.empty())
                sql << " SELECT job_id from t_file where pid=:pid and job_finished is NOT NULL",soci::use(pid), soci::into(job_id);

            soci::statement stmt1 = (
                                        sql.prepare << " SELECT job_state, reuse_job from t_job  "
                                        " WHERE job_id = :job_id ",
                                        soci::use(job_id),
                                        soci::into(currentState),
                                        soci::into(reuseFlag, isNull));
            stmt1.execute(true);

            if(currentState == status)
                return true;

            // total number of file in the job
            soci::statement stmt2 = (
                                        sql.prepare << " SELECT COUNT(DISTINCT file_index) "
                                        " FROM t_file "
                                        " WHERE job_id = :job_id ",
                                        soci::use(job_id),
                                        soci::into(numberOfFilesInJob));
            stmt2.execute(true);


            // number of files that were not canceled
            soci::statement stmt3 = (
                                        sql.prepare << " SELECT COUNT(DISTINCT file_index) "
                                        " FROM t_file "
                                        " WHERE job_id = :jobId "
                                        " 	AND file_state <> 'CANCELED' ", // all the replicas have to be in CANCELED state in order to count a file as canceled
                                        soci::use(job_id),					// so if just one replica is in a different state it is enoght to count it as not canceled
                                        soci::into(numberOfFilesNotCanceled));
            stmt3.execute(true);



            // number of files that were canceled
            numberOfFilesCanceled = numberOfFilesInJob - numberOfFilesNotCanceled;

            // number of files that were finished
            soci::statement stmt4 = (
                                        sql.prepare << " SELECT COUNT(DISTINCT file_index) "
                                        " FROM t_file "
                                        " WHERE job_id = :jobId "
                                        "	AND file_state = 'FINISHED' ", // at least one replica has to be in FINISH state in order to count the file as finished
                                        soci::use(job_id),
                                        soci::into(numberOfFilesFinished));
            stmt4.execute(true);


            // number of files that were not canceled nor failed
            soci::statement stmt5 = (
                                        sql.prepare << " SELECT COUNT(DISTINCT file_index) "
                                        " FROM t_file "
                                        " WHERE job_id = :jobId "
                                        " 	AND file_state <> 'CANCELED' " // for not canceled files see above
                                        " 	AND file_state <> 'FAILED' ",  // all the replicas have to be either in CANCELED or FAILED state in order to count
                                        soci::use(job_id),				   // a files as failed so if just one replica is not in CANCEL neither in FAILED state
                                        soci::into(numberOfFilesNotCanceledNorFailed));
            stmt5.execute(true);

            // number of files that failed
            numberOfFilesFailed = numberOfFilesInJob - numberOfFilesNotCanceledNorFailed - numberOfFilesCanceled;

            // agregated number of files in terminal states (FINISHED, FAILED and CANCELED)
            int numberOfFilesTerminal = numberOfFilesCanceled + numberOfFilesFailed + numberOfFilesFinished;

            bool jobFinished = (numberOfFilesInJob == numberOfFilesTerminal);

            sql.begin();

            if (jobFinished)
                {
                    std::string state;
                    std::string reason = "One or more files failed. Please have a look at the details for more information";
                    if (numberOfFilesFinished > 0 && numberOfFilesFailed > 0)
                        {
                            if (reuseFlag == "H")
                                state = "FAILED";
                            else
                                state = "FINISHEDDIRTY";
                        }
                    else if(numberOfFilesInJob == numberOfFilesFinished)
                        {
                            state = "FINISHED";
                            reason.clear();
                        }
                    else if(numberOfFilesFailed > 0)
                        {
                            state = "FAILED";
                        }
                    else if(numberOfFilesCanceled > 0)
                        {
                            state = "CANCELED";
                        }
                    else
                        {
                            state = "FAILED";
                            reason = "Inconsistent internal state!";
                        }

                    // Update job
                    soci::statement stmt6 = (
                                                sql.prepare << "UPDATE t_job SET "
                                                "    job_state = :state, job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), "
                                                "    reason = :reason "
                                                "WHERE job_id = :jobId AND "
                                                "      job_state NOT IN ('FINISHEDDIRTY','CANCELED','FINISHED','FAILED')",
                                                soci::use(state, "state"), soci::use(reason, "reason"),
                                                soci::use(job_id, "jobId"));
                    stmt6.execute(true);
                }
            // Job not finished yet
            else
                {
                    if (status == "ACTIVE" || status == "STAGING" || status == "SUBMITTED")
                        {
                            soci::statement stmt8 = (
                                                        sql.prepare << "UPDATE t_job "
                                                        "SET job_state = :state "
                                                        "WHERE job_id = :jobId AND"
                                                        "      job_state NOT IN ('FINISHEDDIRTY','CANCELED','FINISHED','FAILED')",
                                                        soci::use(status, "state"), soci::use(job_id, "jobId"));
                            stmt8.execute(true);

                        }
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return ok;
}



void MySqlAPI::updateFileTransferProgressVector(std::vector<struct message_updater>& messages)
{

    soci::session sql(*connectionPool);
    std::vector<struct message_updater> temp;

    try
        {
            double throughput = 0.0;
            double transferred = 0.0;
            int file_id = 0;
            std::string source_surl;
            std::string dest_surl;
            std::string source_turl;
            std::string dest_turl;
            std::string file_state;

            soci::statement stmt = (sql.prepare << "UPDATE t_file SET throughput = :throughput, transferred = :transferred WHERE file_id = :fileId  AND file_state not in ('FINISHED','FAILED','CANCELED') ",
                                    soci::use(throughput), soci::use(transferred), soci::use(file_id));

            soci::statement stmtTurl = (sql.prepare << " INSERT INTO t_turl (source_surl, destin_surl, source_turl, destin_turl, datetime, throughput) "
                                        " VALUES (:source_surl, :destin_surl, :source_turl, :destin_turl, UTC_TIMESTAMP(), :throughput) "
                                        " ON DUPLICATE KEY UPDATE "
                                        " datetime = UTC_TIMESTAMP(), throughput = :throughput",
                                        soci::use(source_surl),
                                        soci::use(dest_surl),
                                        soci::use(source_turl),
                                        soci::use(dest_turl),
                                        soci::use(throughput),
                                        soci::use(throughput)
                                       );

            soci::statement stmtFinish = (sql.prepare << " INSERT INTO t_turl (source_surl, destin_surl, source_turl, destin_turl, datetime, throughput, finish) "
                                          " VALUES (:source_surl, :destin_surl, :source_turl, :destin_turl, UTC_TIMESTAMP(), :throughput, 1) "
                                          " ON DUPLICATE KEY UPDATE "
                                          " datetime = UTC_TIMESTAMP(), throughput = :throughput, finish = finish + 1",
                                          soci::use(source_surl),
                                          soci::use(dest_surl),
                                          soci::use(source_turl),
                                          soci::use(dest_turl),
                                          soci::use(throughput),
                                          soci::use(throughput)
                                         );

            soci::statement stmtFail = (sql.prepare << " INSERT INTO t_turl (source_surl, destin_surl, source_turl, destin_turl, datetime, throughput, fail) "
                                        " VALUES (:source_surl, :destin_surl, :source_turl, :destin_turl, UTC_TIMESTAMP(), :throughput, 1) "
                                        " ON DUPLICATE KEY UPDATE "
                                        " datetime = UTC_TIMESTAMP(), throughput = :throughput, fail = fail + 1",
                                        soci::use(source_surl),
                                        soci::use(dest_surl),
                                        soci::use(source_turl),
                                        soci::use(dest_turl),
                                        soci::use(throughput),
                                        soci::use(throughput)
                                       );

            soci::statement stmtFileid = (sql.prepare << "select file_state from t_file where file_id=:file_id", soci::use(file_id), soci::into(file_state));

            sql.begin();

            std::vector<struct message_updater>::iterator iter;
            for (iter = messages.begin(); iter != messages.end(); ++iter)
                {
                    throughput = 0.0;
                    transferred = 0.0;
                    file_id = 0;

                    if (iter->msg_errno == 0)
                        {
                            if((*iter).throughput > 0.0)
                                {
                                    throughput = convertKbToMb((*iter).throughput);
                                    transferred = (*iter).transferred;
                                    file_id = (*iter).file_id;
                                    stmt.execute(true);
                                }
                            temp.push_back((*iter));
                        }
                }

            sql.commit();

            //now update t_turl table by checking file state
            sql.begin();

            for (iter = temp.begin(); iter != temp.end(); ++iter)
                {
                    file_state = ""; //reset all
                    source_surl = "";
                    dest_surl = "";
                    source_turl = "";
                    dest_turl = "";
                    throughput = 0.0;

                    if (iter->msg_errno == 0)
                        {	
                            source_surl = (*iter).source_surl;
                            dest_surl = (*iter).dest_surl;
                            source_turl = (*iter).source_turl;
                            dest_turl = (*iter).dest_turl;			
			
			    if(source_turl == "gsiftp:://fake" && dest_turl == "gsiftp:://fake")
                                continue;
				
                            if((*iter).throughput > 0.0)
                                {
                                    throughput = convertKbToMb((*iter).throughput);
                                }			    

                            file_id = (*iter).file_id;

                            //check file state for tis file_id
                            stmtFileid.execute(true);

                            if(file_state == "FINISHED")
                                {
                                    stmtFinish.execute(true);
                                }
                            else if (file_state == "FAILED")
                                {
                                    stmtFail.execute(true);
                                }
                            else
                                {
                                    stmtTurl.execute(true);
                                }
                        }
                }
		
            sql.commit();
            temp.clear();
        }
    catch (std::exception& e)
        {
            temp.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            temp.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::cancelJob(std::vector<std::string>& requestIDs)
{
    soci::session sql(*connectionPool);
    const std::string reason = "Job canceled by the user";
    std::string job_id;
    std::ostringstream cancelStmt1;
    std::ostringstream cancelStmt2;
    std::ostringstream jobIdStmt;

    try
        {
            for (std::vector<std::string>::const_iterator i = requestIDs.begin(); i != requestIDs.end(); ++i)
                {
                    job_id = (*i);
                    jobIdStmt << "'";
                    jobIdStmt << job_id;
                    jobIdStmt << "',";
                }

            std::string queryStr = jobIdStmt.str();
            job_id = queryStr.substr(0, queryStr.length() - 1);

            cancelStmt1 << "UPDATE t_job SET job_state = 'CANCELED', job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), cancel_job='Y' ";
            cancelStmt1 << " ,reason = '";
            cancelStmt1 << reason;
            cancelStmt1 << "'";
            cancelStmt1 << " WHERE job_id IN (";
            cancelStmt1 << job_id;
            cancelStmt1 << ")";
            cancelStmt1 << " AND job_state NOT IN ('CANCELED','FINISHEDDIRTY', 'FINISHED', 'FAILED')";

            cancelStmt2 << "UPDATE t_file SET file_state = 'CANCELED',  finish_time = UTC_TIMESTAMP() ";
            cancelStmt2 << " ,reason = '";
            cancelStmt2 << reason;
            cancelStmt2 << "'";
            cancelStmt2 << " WHERE job_id IN (";
            cancelStmt2 << job_id;
            cancelStmt2 << ")";
            cancelStmt2 << " AND file_state NOT IN ('CANCELED','FINISHED','FAILED')";

            soci::statement stmt1 = (sql.prepare << cancelStmt1.str());
            soci::statement stmt2 = (sql.prepare << cancelStmt2.str());


            sql.begin();
            // Cancel job
            stmt1.execute(true);

            // Cancel files
            stmt2.execute(true);
            sql.commit();

            jobIdStmt.str(std::string());
            jobIdStmt.clear();
            cancelStmt1.str(std::string());
            cancelStmt1.clear();
            cancelStmt2.str(std::string());
            cancelStmt2.clear();

        }
    catch (std::exception& e)
        {
            jobIdStmt.str(std::string());
            jobIdStmt.clear();
            cancelStmt1.str(std::string());
            cancelStmt1.clear();
            cancelStmt2.str(std::string());
            cancelStmt2.clear();

            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            jobIdStmt.str(std::string());
            jobIdStmt.clear();
            cancelStmt1.str(std::string());
            cancelStmt1.clear();
            cancelStmt2.str(std::string());
            cancelStmt2.clear();

            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::getCancelJob(std::vector<int>& requestIDs)
{
    soci::session sql(*connectionPool);
    int pid = 0;
    int file_id = 0;

    try
        {
            soci::rowset<soci::row> rs = (sql.prepare << " select distinct pid, file_id from t_file where PID IS NOT NULL AND file_state='CANCELED' and job_finished is NULL AND (hashed_id >= :hStart AND hashed_id <= :hEnd) ",
                                          soci::use(hashSegment.start), soci::use(hashSegment.end));

            soci::statement stmt1 = (sql.prepare << "UPDATE t_file SET  job_finished = UTC_TIMESTAMP() "
                                     "WHERE pid = :pid AND file_id = :file_id ",
                                     soci::use(pid, "pid"), soci::use(file_id, "file_id"));

            // Cancel files
            sql.begin();
            for (soci::rowset<soci::row>::const_iterator i2 = rs.begin(); i2 != rs.end(); ++i2)
                {
                    soci::row const& row = *i2;
                    pid = row.get<int>("pid");
                    file_id = row.get<int>("file_id");
                    requestIDs.push_back(pid);

                    stmt1.execute(true);
                }
            sql.commit();


            //now set job_finished to all files not having pid set
            //prevent more than on server to update the optimizer decisions
            if(hashSegment.start == 0)
                {
                    file_id = 0;

                    soci::rowset<soci::row> rs2 = (sql.prepare << " select distinct file_id from t_file where file_state='CANCELED' and PID IS NULL and (job_finished is NULL or finish_time is NULL)");

                    soci::statement stmt2 = (sql.prepare << "UPDATE t_file SET job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP()  WHERE file_id=:file_id ", soci::use(file_id, "file_id"));

                    sql.begin();
                    for (soci::rowset<soci::row>::const_iterator i2 = rs2.begin(); i2 != rs2.end(); ++i2)
                        {
                            soci::row const& row = *i2;
                            file_id = row.get<int>("file_id");
                            stmt2.execute(true);
                        }
                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




/*t_credential API*/
bool MySqlAPI::insertGrDPStorageCacheElement(std::string dlg_id, std::string dn, std::string cert_request, std::string priv_key, std::string voms_attrs)
{
    soci::session sql(*connectionPool);


    try
        {
            sql.begin();
            sql << "INSERT INTO t_credential_cache "
                "    (dlg_id, dn, cert_request, priv_key, voms_attrs) VALUES "
                "    (:dlgId, :dn, :certRequest, :privKey, :vomsAttrs)",
                soci::use(dlg_id), soci::use(dn), soci::use(cert_request), soci::use(priv_key), soci::use(voms_attrs);
            sql.commit();
        }
    catch (soci::mysql_soci_error const &e)
        {
            sql.rollback();
            unsigned int err_code = e.err_num_;

            // the magic '1062' is the error code of
            // Duplicate entry 'XXX' for key 'PRIMARY'
            if (err_code == 1062) return false;

            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return true;
}



void MySqlAPI::updateGrDPStorageCacheElement(std::string dlg_id, std::string dn, std::string cert_request, std::string priv_key, std::string voms_attrs)
{
    soci::session sql(*connectionPool);

    try
        {
            soci::statement stmt(sql);

            stmt.exchange(soci::use(cert_request, "certRequest"));
            stmt.exchange(soci::use(priv_key, "privKey"));
            stmt.exchange(soci::use(voms_attrs, "vomsAttrs"));
            stmt.exchange(soci::use(dlg_id, "dlgId"));
            stmt.exchange(soci::use(dn, "dn"));

            stmt.alloc();

            stmt.prepare("UPDATE t_credential_cache SET "
                         "    cert_request = :certRequest, "
                         "    priv_key = :privKey, "
                         "    voms_attrs = :vomsAttrs "
                         "WHERE dlg_id = :dlgId AND dn=:dn");

            stmt.define_and_bind();

            sql.begin();
            stmt.execute(true);
            if (stmt.get_affected_rows() == 0)
                {
                    std::ostringstream msg;
                    msg << "No entries updated in t_credential_cache! "
                        << dn << " (" << dlg_id << ")";
                    throw Err_Custom(msg.str());
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



CredCache* MySqlAPI::findGrDPStorageCacheElement(std::string delegationID, std::string dn)
{
    CredCache* cred = NULL;
    soci::session sql(*connectionPool);

    try
        {
            cred = new CredCache();
            sql << "SELECT dlg_id, dn, voms_attrs, cert_request, priv_key "
                "FROM t_credential_cache "
                "WHERE dlg_id = :dlgId and dn = :dn",
                soci::use(delegationID), soci::use(dn), soci::into(*cred);
            if (!sql.got_data())
                {
                    delete cred;
                    cred = NULL;
                }
        }
    catch (std::exception& e)
        {
            if(cred)
                delete cred;
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            if(cred)
                delete cred;
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return cred;
}



void MySqlAPI::deleteGrDPStorageCacheElement(std::string delegationID, std::string dn)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "DELETE FROM t_credential_cache WHERE dlg_id = :dlgId AND dn = :dn",
                soci::use(delegationID), soci::use(dn);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::insertGrDPStorageElement(std::string dlg_id, std::string dn, std::string proxy, std::string voms_attrs, time_t termination_time)
{
    soci::session sql(*connectionPool);

    try
        {
            struct tm tTime;
            gmtime_r(&termination_time, &tTime);

            sql.begin();
            sql << "INSERT INTO t_credential "
                "    (dlg_id, dn, termination_time, proxy, voms_attrs) VALUES "
                "    (:dlgId, :dn, :terminationTime, :proxy, :vomsAttrs)",
                soci::use(dlg_id), soci::use(dn), soci::use(tTime), soci::use(proxy), soci::use(voms_attrs);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::updateGrDPStorageElement(std::string dlg_id, std::string dn, std::string proxy, std::string voms_attrs, time_t termination_time)
{
    soci::session sql(*connectionPool);


    try
        {
            sql.begin();
            struct tm tTime;
            gmtime_r(&termination_time, &tTime);

            sql << "UPDATE t_credential SET "
                "    proxy = :proxy, "
                "    voms_attrs = :vomsAttrs, "
                "    termination_time = :terminationTime "
                "WHERE dlg_id = :dlgId AND dn = :dn",
                soci::use(proxy), soci::use(voms_attrs), soci::use(tTime),
                soci::use(dlg_id), soci::use(dn);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



Cred* MySqlAPI::findGrDPStorageElement(std::string delegationID, std::string dn)
{
    Cred* cred = NULL;
    soci::session sql(*connectionPool);

    try
        {
            cred = new Cred();
            sql << "SELECT dlg_id, dn, voms_attrs, proxy, termination_time "
                "FROM t_credential "
                "WHERE dlg_id = :dlgId AND dn =:dn",
                soci::use(delegationID), soci::use(dn),
                soci::into(*cred);

            if (!sql.got_data())
                {
                    delete cred;
                    cred = NULL;
                }
        }
    catch (std::exception& e)
        {
            if(cred)
                delete cred;
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            if(cred)
                delete cred;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return cred;
}



void MySqlAPI::deleteGrDPStorageElement(std::string delegationID, std::string dn)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "DELETE FROM t_credential WHERE dlg_id = :dlgId AND dn = :dn",
                soci::use(delegationID), soci::use(dn);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



bool MySqlAPI::getDebugMode(std::string source_hostname, std::string destin_hostname)
{
    soci::session sql(*connectionPool);

    bool isDebug = false;
    try
        {
            std::string debug;
            sql <<
                " SELECT debug "
                " FROM t_debug "
                " WHERE source_se = :source "
                "	AND (dest_se = '' OR dest_se IS NULL) ",
                soci::use(source_hostname),
                soci::into(debug)
                ;

            isDebug = (debug == "on");
            if (isDebug) return isDebug;

            sql <<
                " SELECT debug "
                " FROM t_debug "
                " WHERE source_se = :destin "
                "	AND (dest_se = '' OR dest_se IS NULL) ",
                soci::use(destin_hostname),
                soci::into(debug)
                ;

            isDebug = (debug == "on");
            if (isDebug) return isDebug;

            sql << "SELECT debug FROM t_debug WHERE source_se = :source AND dest_se = :dest",
                soci::use(source_hostname), soci::use(destin_hostname), soci::into(debug);

            isDebug = (debug == "on");
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return isDebug;
}



void MySqlAPI::setDebugMode(std::string source_hostname, std::string destin_hostname, std::string mode)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            if (destin_hostname.length() == 0)
                {
                    sql << "DELETE FROM t_debug WHERE source_se = :source AND dest_se IS NULL",
                        soci::use(source_hostname);
                    sql << "INSERT INTO t_debug (source_se, debug) VALUES (:source, :debug)",
                        soci::use(source_hostname), soci::use(mode);
                }
            else
                {
                    sql << "DELETE FROM t_debug WHERE source_se = :source AND dest_se = :dest",
                        soci::use(source_hostname), soci::use(destin_hostname);
                    sql << "INSERT INTO t_debug (source_se, dest_se, debug) VALUES (:source, :dest, :mode)",
                        soci::use(source_hostname), soci::use(destin_hostname), soci::use(mode);
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::getSubmittedJobsReuse(std::vector<TransferJobs*>& jobs, const std::string &)
{
    soci::session sql(*connectionPool);

    try
        {
            soci::rowset<TransferJobs> rs = (sql.prepare << "SELECT "
                                             "   job_id, "
                                             "   job_state, "
                                             "   vo_name,  "
                                             "   priority,  "
                                             "   source_se, "
                                             "   dest_se,  "
                                             "   agent_dn, "
                                             "   submit_host, "
                                             "   user_dn, "
                                             "   user_cred, "
                                             "   cred_id,  "
                                             "   space_token, "
                                             "   storage_class,  "
                                             "   job_params, "
                                             "   overwrite_flag, "
                                             "   source_space_token, "
                                             "   source_token_description,"
                                             "   copy_pin_lifetime, "
                                             "   checksum_method, "
                                             "   bring_online, "
                                             "   submit_time, "
                                             "   reuse_job "
                                             "   FROM t_job WHERE "
                                             "   job_state = 'SUBMITTED' AND job_finished IS NULL AND "
                                             "   cancel_job IS NULL AND "
                                             "   (reuse_job IS NOT NULL AND reuse_job != 'N') "
                                             " ORDER BY priority DESC, submit_time LIMIT 1 ");

            for (soci::rowset<TransferJobs>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    TransferJobs const& tjob = *i;
                    jobs.push_back(new TransferJobs(tjob));
                }
        }
    catch (std::exception& e)
        {
            std::vector<TransferJobs*>::iterator iter2;
            for (iter2 = jobs.begin(); iter2 != jobs.end(); ++iter2)
                {
                    if(*iter2)
                        delete *iter2;
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            std::vector<TransferJobs*>::iterator iter2;
            for (iter2 = jobs.begin(); iter2 != jobs.end(); ++iter2)
                {
                    if(*iter2)
                        delete *iter2;
                }
            jobs.clear();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}





void MySqlAPI::auditConfiguration(const std::string & dn, const std::string & config, const std::string & action)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "INSERT INTO t_config_audit (datetime, dn, config, action ) VALUES "
                "                           (UTC_TIMESTAMP(), :dn, :config, :action)",
                soci::use(dn), soci::use(config), soci::use(action);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



/*custom optimization stuff*/

void MySqlAPI::fetchOptimizationConfig2(OptimizerSample* ops, const std::string & /*source_hostname*/, const std::string & /*destin_hostname*/)
{
    ops->streamsperfile = DEFAULT_NOSTREAMS;
    ops->timeout = MID_TIMEOUT;
    ops->bufsize = DEFAULT_BUFFSIZE;
}




bool MySqlAPI::isCredentialExpired(const std::string & dlg_id, const std::string & dn)
{
    soci::session sql(*connectionPool);

    bool expired = true;
    try
        {
            struct tm termTime;

            sql << "SELECT termination_time FROM t_credential WHERE dlg_id = :dlgId AND dn = :dn",
                soci::use(dlg_id), soci::use(dn), soci::into(termTime);

            if (sql.got_data())
                {
                    time_t termTimestamp = timegm(&termTime);
                    time_t now = getUTC(0);
                    expired = (difftime(termTimestamp, now) <= 0);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return !expired;
}

bool MySqlAPI::getMaxActive(soci::session& sql, int active, int highDefault, const std::string & source_hostname, const std::string & destin_hostname)
{
    long long int maxActiveSource = 0;
    long long int maxActiveDest = 0;
    soci::indicator isNullmaxActiveSource = soci::i_ok;
    soci::indicator isNullmaxActiveDest = soci::i_ok;

    try
        {
            sql << " select active from t_optimize where source_se = :source_se and active is not NULL ",
                soci::use(source_hostname),
                soci::into(maxActiveSource, isNullmaxActiveSource);

            if (sql.got_data() && active > maxActiveSource)
                return false;

            sql << " select active from t_optimize where dest_se = :dest_se and active is not NULL ",
                soci::use(destin_hostname),
                soci::into(maxActiveDest, isNullmaxActiveDest);

            //check limits for dest
            if (sql.got_data() && active > maxActiveDest)
                return false;

            if(active >= MAX_ACTIVE_PER_LINK)
                {
                    return false;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return true;
}

bool MySqlAPI::isTrAllowed(const std::string & source_hostname, const std::string & destin_hostname)
{
    soci::session sql(*connectionPool);

    int maxActive = 0;
    int active = 0;
    bool allowed = false;
    soci::indicator isNull = soci::i_ok;

    try
        {
            int highDefault = getOptimizerMode(sql);

            soci::statement stmt1 = (
                                        sql.prepare << "SELECT active FROM t_optimize_active "
                                        "WHERE source_se = :source AND dest_se = :dest_se ",
                                        soci::use(source_hostname),soci::use(destin_hostname), soci::into(maxActive, isNull));
            stmt1.execute(true);

            soci::statement stmt2 = (
                                        sql.prepare << "SELECT count(*) FROM t_file "
                                        "WHERE source_se = :source AND dest_se = :dest_se and file_state in ('READY','ACTIVE') ",
                                        soci::use(source_hostname),soci::use(destin_hostname), soci::into(active));
            stmt2.execute(true);

            if (isNull != soci::i_null)
                {
                    if(active < maxActive)
                        allowed = true;
                }

            if(active < highDefault)
                {
                    allowed = true;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return allowed;
}

bool MySqlAPI::bandwidthChecker(soci::session& sql, const std::string & source_hostname, const std::string & destination_hostname, int& bandwidthIn)
{
    long long int bandwidthSrc = 0;
    long long int bandwidthDst = 0;
    double througputSrc = 0.0;
    double througputDst = 0.0;

    soci::indicator isNullBandwidthSrc = soci::i_ok;
    soci::indicator isNullBandwidthDst = soci::i_ok;
    soci::indicator isNullThrougputSrc = soci::i_ok;
    soci::indicator isNullThrougputDst = soci::i_ok;

    //get limit for source
    sql << "select throughput from t_optimize where source_se= :name and throughput is not NULL ",
        soci::use(source_hostname), soci::into(bandwidthSrc, isNullBandwidthSrc);

    //get limit for dest
    sql << "select throughput from t_optimize where dest_se= :name  and throughput is not NULL",
        soci::use(destination_hostname), soci::into(bandwidthDst, isNullBandwidthDst);

    if(!sql.got_data() || bandwidthSrc == -1)
        bandwidthSrc = -1;

    if(!sql.got_data() || bandwidthDst == -1)
        bandwidthDst = -1;

    //no limits are applied either for source or dest, stop here before executing more expensive queries
    if(bandwidthDst == -1 && bandwidthSrc == -1)
        {
            return true;
        }

    //get aggregated thr from source
    sql << "select sum(throughput) from t_file where source_se= :name and file_state='ACTIVE' and throughput is not NULL ",
        soci::use(source_hostname), soci::into(througputSrc, isNullThrougputSrc);

    //get aggregated thr towards dest
    sql << "select sum(throughput) from t_file where dest_se= :name and file_state='ACTIVE' and throughput is not NULL",
        soci::use(destination_hostname), soci::into(througputDst, isNullThrougputDst);


    if(bandwidthSrc > 0 )
        {
            if(bandwidthDst > 0) //both source and dest have limits, take the lowest
                {
                    //get the lowest limit to be respected
                    double lowest = (bandwidthSrc < bandwidthDst) ? bandwidthSrc : bandwidthDst;

                    if (througputSrc > lowest || througputDst > lowest)
                        {
                            bandwidthIn = lowest;
                            FTS3_COMMON_LOGGER_NEWLOG(WARNING) << "Bandwidth limitation of " << lowest  << " MB/s is set for " << source_hostname << " or " <<  destination_hostname << commit;
                            return false;
                        }
                }
            //only source limit is set
            if (througputSrc > bandwidthSrc)
                {
                    bandwidthIn = bandwidthSrc;
                    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << "Bandwidth limitation of " << bandwidthSrc  << " MB/s is set for " << source_hostname << commit;
                    return false;
                }

        }
    else if(bandwidthDst > 0)  //only destination has limit
        {
            bandwidthIn = bandwidthDst;

            if(througputDst > bandwidthDst)
                {
                    FTS3_COMMON_LOGGER_NEWLOG(WARNING) << "Bandwidth limitation of " << bandwidthDst  << " MB/s is set for " << destination_hostname << commit;
                    return false;
                }
        }
    else
        {
            return true;
        }

    return true;
}



bool MySqlAPI::updateOptimizer()
{
    //prevent more than on server to update the optimizer decisions
    if(hashSegment.start != 0)
        return false;

    soci::session sql(*connectionPool);

    int allowed = false;
    std::string source_hostname;
    std::string destin_hostname;
    int active = 0;
    int maxActive = 0;
    soci::indicator isNullRetry = soci::i_ok;
    soci::indicator isNullMaxActive = soci::i_ok;
    soci::indicator isNullRate = soci::i_ok;
    double retry = 0.0;   //latest from db
    double lastSuccessRate = 0.0;
    int retrySet = 0;
    soci::indicator isRetry = soci::i_ok;
    soci::indicator isNullStreamsFound = soci::i_ok;
    long long streamsFound = 0;
    long long recordFound = 0;
    int insertStreams = -1;
    soci::indicator isNullStartTime = soci::i_ok;
    soci::indicator isNullRecordsFound = soci::i_ok;
    long long int streamsCurrent = 0;
    soci::indicator isNullStreamsCurrent = soci::i_ok;
    long long singleDest = 0;
    bool lanTransferBool = false;

    time_t now = getUTC(0);
    struct tm startTimeSt;


    try
        {
            //check optimizer level, minimum active per link
            int highDefault = getOptimizerMode(sql);

            //store the default
            int tempDefault =   highDefault;

            //based on the level, how many transfers will be spawned
            int spawnActive = getOptimizerDefaultMode(sql);

            //fetch the records from db for distinct links
            soci::rowset<soci::row> rs = ( sql.prepare <<
                                           " select  distinct o.source_se, o.dest_se from t_optimize_active o INNER JOIN "
                                           " t_file f ON (o.source_se = f.source_se) where o.dest_se=f.dest_se and "
                                           " f.file_state in ('SUBMITTED','ACTIVE') and f.job_finished is null ");

            //snapshot of active transfers
            soci::statement stmt7 = (
                                        sql.prepare << "SELECT count(*) FROM t_file "
                                        "WHERE source_se = :source AND dest_se = :dest_se and file_state in ('READY','ACTIVE') ",
                                        soci::use(source_hostname),soci::use(destin_hostname), soci::into(active));

            //max number of active allowed per link
            soci::statement stmt8 = (
                                        sql.prepare << "SELECT active FROM t_optimize_active "
                                        "WHERE source_se = :source AND dest_se = :dest_se LIMIT 1",
                                        soci::use(source_hostname),soci::use(destin_hostname), soci::into(maxActive, isNullMaxActive));

            //sum of retried transfers per link
            soci::statement stmt9 = (
                                        sql.prepare << "select sum(retry) from t_file WHERE source_se = :source AND dest_se = :dest_se and "
                                        "file_state in ('READY','ACTIVE','SUBMITTED') order by start_time DESC LIMIT 50 ",
                                        soci::use(source_hostname),soci::use(destin_hostname), soci::into(retry, isNullRetry));

            soci::statement stmt10 = (
                                         sql.prepare << "update t_optimize_active set active=:active where "
                                         " source_se=:source and dest_se=:dest and (datetime is NULL OR datetime >= (UTC_TIMESTAMP() - INTERVAL '50' second)) ",
                                         soci::use(active), soci::use(source_hostname), soci::use(destin_hostname));


            soci::statement stmt12 = (
                                         sql.prepare << " select datetime from t_optimize where  "
                                         " source_se=:source_se and dest_se=:dest_se LIMIT 1 ",
                                         soci::use(source_hostname), soci::use(destin_hostname), soci::into(startTimeSt, isNullStartTime));

            soci::statement stmt13 = (
                                         sql.prepare << "update t_optimize set nostreams=:nostreams where "
                                         " source_se=:source and dest_se=:dest ",
                                         soci::use(insertStreams),
                                         soci::use(source_hostname),
                                         soci::use(destin_hostname));

            soci::statement stmt14 = (
                                         sql.prepare << " select count(*) from t_optimize where "
                                         " source_se=:source_se and dest_se=:dest_se ",
                                         soci::use(source_hostname), soci::use(destin_hostname), soci::into(recordFound, isNullRecordsFound));

            soci::statement stmt15 = (
                                         sql.prepare << "insert into t_optimize (source_se, dest_se, nostreams, datetime) "
                                         " VALUES (:source_se, :dest_se, :nostreams, UTC_TIMESTAMP())",
                                         soci::use(source_hostname),
                                         soci::use(destin_hostname),
                                         soci::use(insertStreams));

            soci::statement stmt16 = (
                                         sql.prepare << "update t_optimize set datetime=UTC_TIMESTAMP() where "
                                         " source_se=:source and dest_se=:dest ",
                                         soci::use(source_hostname),
                                         soci::use(destin_hostname));

            soci::statement stmt17 = (
                                         sql.prepare << " select nostreams from t_optimize where "
                                         " source_se=:source_se and dest_se=:dest_se",
                                         soci::use(source_hostname), soci::use(destin_hostname), soci::into(streamsCurrent, isNullStreamsCurrent));

            soci::statement stmt18 = (
                                         sql.prepare << " select count(distinct source_se) from t_file where "
                                         " file_state in ('ACTIVE','READY','SUBMITTED') and "
                                         " dest_se=:dest and "
                                         " job_finished is null",
                                         soci::use(destin_hostname), soci::into(singleDest));


            //check if retry is set at global level
            sql <<
                " SELECT retry "
                " FROM t_server_config LIMIT 1",soci::into(retrySet, isRetry)
                ;

            //if not set, flag as 0
            if (isRetry == soci::i_null || retrySet == 0)
                retrySet = 0;


            for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    source_hostname = i->get<std::string>("source_se");
                    destin_hostname = i->get<std::string>("dest_se");

                    double nFailedLastHour=0.0, nFinishedLastHour=0.0;
                    double throughput=0.0;
                    double filesize = 0.0;
                    double totalSize = 0.0;
                    retry = 0.0;   //latest from db
                    double retryStored = 0.0; //stored in mem
                    double thrStored = 0.0; //stored in mem
                    double rateStored = 0.0; //stored in mem
                    int activeStored = 0; //stored in mem
                    int thrSamplesStored = 0; //stored in mem
                    int throughputSamples = 0;
                    double ratioSuccessFailure = 0.0;
                    active = 0;
                    maxActive = 0;
                    isNullRetry = soci::i_ok;
                    isNullMaxActive = soci::i_ok;
                    lastSuccessRate = 0.0;
                    isNullRate = soci::i_ok;
                    isNullStreamsFound = soci::i_ok;
                    streamsFound = 0;
                    recordFound = 0;
                    insertStreams = -1;
                    isNullStartTime = soci::i_ok;
                    isNullRecordsFound = soci::i_ok;
                    streamsCurrent = 0;
                    isNullStreamsCurrent = soci::i_ok;
                    singleDest = 0;
                    lanTransferBool = false;
                    now = getUTC(0);

                    if(true == lanTransfer(source_hostname, destin_hostname))
                        {
                            highDefault = (highDefault * 3);
                            lanTransferBool = true;
                        }
                    else //default
                        {
                            highDefault = tempDefault;
                        }

                    // check current active transfers for a linkmaxActive
                    stmt7.execute(true);

                    //check if there is any other source for a given dest
                    stmt18.execute(true);

                    // Weighted average
                    soci::rowset<soci::row> rsSizeAndThroughput = (sql.prepare <<
                            " SELECT filesize, throughput "
                            " FROM t_file "
                            " WHERE source_se = :source AND dest_se = :dest AND "
                            "       file_state  in ('ACTIVE','FINISHED') AND throughput > 0 AND "
                            "       filesize > 0 and (job_finished is null or job_finished >= (UTC_TIMESTAMP() - interval '1' minute))",
                            soci::use(source_hostname),soci::use(destin_hostname));

                    for (soci::rowset<soci::row>::const_iterator j = rsSizeAndThroughput.begin();
                            j != rsSizeAndThroughput.end(); ++j)
                        {
                            filesize    = j->get<double>("filesize", 0.0);
                            throughput += (j->get<double>("throughput", 0.0) * filesize);
                            totalSize  += filesize;
                        }
                    if (totalSize > 0)
                        {
                            throughput /= totalSize;
                        }

                    // Ratio of success
                    soci::rowset<soci::row> rs = (sql.prepare << "SELECT file_state, retry, current_failures FROM t_file "
                                                  "WHERE "
                                                  "      t_file.source_se = :source AND t_file.dest_se = :dst AND "
                                                  "      (t_file.job_finished is NULL OR t_file.job_finished > (UTC_TIMESTAMP() - interval '1' minute)) AND "
                                                  "      file_state IN ('FAILED','FINISHED','SUBMITTED') ",
                                                  soci::use(source_hostname), soci::use(destin_hostname));


                    //we need to exclude non-recoverable errors so as not to count as failures and affect effiency
                    for (soci::rowset<soci::row>::const_iterator i = rs.begin();
                            i != rs.end(); ++i)
                        {
                            std::string state = i->get<std::string>("file_state", "");
                            int retryNum = i->get<int>("retry", 0);
                            int current_failures = i->get<int>("current_failures", 0);

                            if(state.compare("FAILED") == 0 && current_failures == 0)
                                {
                                    //do nothing, it's a non recoverable error so do not consider it
                                }
                            else if ( (state.compare("FAILED") == 0 ||  state.compare("SUBMITTED") == 0) && retrySet > 0 && retryNum > 0)
                                {
                                    nFailedLastHour+=1.0;
                                }
                            else if (state.compare("FAILED") == 0 && retrySet == 0)
                                {
                                    nFailedLastHour+=1.0;
                                }
                            else if (state.compare("FINISHED") == 0)
                                {
                                    nFinishedLastHour+=1.0;
                                }
                        }

                    //round up efficiency
                    if(nFinishedLastHour > 0.0)
                        {
                            ratioSuccessFailure = ceil(nFinishedLastHour/(nFinishedLastHour + nFailedLastHour) * (100.0/1.0));
                        }

                    //optimize number of streams first
                    //check if pair exists first
                    stmt14.execute(true);

                    if(recordFound == 0) //insert new pair
                        {
                            if(throughput != 0.0 && throughput < 1.0) //records found, optimize number of streams
                                {
                                    insertStreams = -1;
                                    sql.begin();
                                    stmt15.execute(true);
                                    sql.commit();
                                }
                            else
                                {
                                    insertStreams = 4; //use auto-tuning, pair doesn't need optimization
                                    sql.begin();
                                    stmt15.execute(true);
                                    sql.commit();
                                }
                        }
                    else   //update existing pair
                        {
                            //check if 6h elapsed so as to experiment with auto-tune
                            stmt12.execute(true);
                            time_t startTime = timegm(&startTimeSt);
                            double diff = 0.0;
                            if(isNullStartTime != soci::i_null)
                                {
                                    diff = difftime(now, startTime);
                                }

                            //get current nostreams for this pair
                            stmt17.execute(true);

                            if(isNullStreamsCurrent == soci::i_null)  //if null, set to -1, shouldn't be but never know!
                                {
                                    streamsCurrent = -1;
                                }

                            if(throughput != 0.0 && throughput < 1.0) //records found, optimize number of streams by reducing them
                                {
                                    if(diff > 10000) //if elapsed, fall-back to auto-tune
                                        {
                                            insertStreams = 4;
                                            sql.begin();
                                            stmt13.execute(true);
                                            stmt16.execute(true);
                                            sql.commit();
                                        }
                                    else
                                        {
                                            insertStreams = -1;
                                            sql.begin();
                                            stmt13.execute(true);
                                            sql.commit();
                                        }
                                }
                            else if (throughput != 0.0 && throughput >= 1.0 && streamsCurrent == -1)
                                {
                                    if(diff > 10000) //if elapsed, fall-back to auto-tune
                                        {
                                            insertStreams = 4;
                                            sql.begin();
                                            stmt13.execute(true);
                                            stmt16.execute(true);
                                            sql.commit();
                                        }
                                    else
                                        {
                                            insertStreams = -1;
                                            sql.begin();
                                            stmt13.execute(true);
                                            sql.commit();
                                        }
                                }
                            else if (throughput != 0.0 && throughput >= 1.0 && streamsCurrent != -1) //auto-tune working fine
                                {
                                    insertStreams = 4;
                                    sql.begin();
                                    stmt13.execute(true);
                                    sql.commit();
                                }
                            else
                                {
                                    insertStreams = 4;
                                    sql.begin();
                                    stmt13.execute(true);
                                    sql.commit();
                                }
                        }

                    // Max active transfers
                    stmt8.execute(true);

                    //check if have been retried
                    if (retrySet > 0)
                        {
                            stmt9.execute(true);
                            if (isNullRetry == soci::i_null)
                                retry = 0;
                        }

                    if (isNullMaxActive == soci::i_null)
                        maxActive = highDefault;

                    //only apply the logic below if any of these values changes
                    bool changed = getChangedFile (source_hostname, destin_hostname, ratioSuccessFailure, rateStored, throughput, thrStored, retry, retryStored, maxActive, activeStored, throughputSamples, thrSamplesStored);
                    if(!changed && retry > 0)
                        changed = true;

                    //check if bandwidth limitation exists, if exists and throughput exceeds the limit then do not proccess with auto-tuning
                    int bandwidthIn = 0;
                    bool bandwidth = bandwidthChecker(sql, source_hostname, destin_hostname, bandwidthIn);

                    //make sure bandwidth is respected as also active should be no less than the minimum for each link
                    if(!bandwidth)
                        {
                            sql.begin();

                            active = ((maxActive - 2) < highDefault)? highDefault: (maxActive - 2);
                            stmt10.execute(true);
                            updateOptimizerEvolution(sql, source_hostname, destin_hostname, active, throughput, ratioSuccessFailure, 10, bandwidthIn);

                            sql.commit();

                            continue;
                        }

                    //ratioSuccessFailure, rateStored, throughput, thrStored MUST never be zero
                    if(changed)
                        {
                            sql.begin();

                            int pathFollowed = 0;

                            if( (ratioSuccessFailure == 100 || (ratioSuccessFailure > rateStored && ratioSuccessFailure > 98)) && throughput > thrStored && retry <= retryStored)
                                {
				    int tempActive = active; //temp store current active
				    
                                    //make sure we do not increase beyond limits set
                                    bool maxActiveLimit = getMaxActive(sql, maxActive, highDefault, source_hostname, destin_hostname);

                                    if(maxActiveLimit) // no limit
                                        {
                                            if(singleDest == 1)
                                                {
                                                    active = maxActive + spawnActive + 1;
                                                }
                                            else if (lanTransferBool)
                                                {
                                                    active = maxActive + spawnActive + 1;
                                                }
                                            else
                                                {
                                                    active = maxActive + spawnActive;
                                                }
						
				           if(active > (tempActive + 10))
					        {
						    active = maxActive;						    
						}

                                            pathFollowed = 1;
                                            stmt10.execute(true);
                                        }
                                    else //no change, max limit reached
                                        {
                                            active = maxActive;
                                            pathFollowed = 11;
                                            stmt10.execute(true);
                                        }
                                }
                            else if( (ratioSuccessFailure == 100 || (ratioSuccessFailure > rateStored && ratioSuccessFailure > 98)) && throughput == thrStored && retry <= retryStored)
                                {
                                    active = maxActive;
                                    pathFollowed = 2;

                                    stmt10.execute(true);
                                }
                            else if( (ratioSuccessFailure == 100 || (ratioSuccessFailure > rateStored && ratioSuccessFailure > 95)) && throughput < thrStored)
                                {
                                    if(retry > retryStored)
                                        {
                                            active = ((maxActive - 1) < highDefault)? highDefault: (maxActive - 1);
                                            pathFollowed = 3;
                                        }
                                    else if(thrSamplesStored == 3)
                                        {
                                            active = ((maxActive - 1) < highDefault)? highDefault: (maxActive - 1);
                                            pathFollowed = 4;
                                        }
                                    else
                                        {
                                            if(maxActive >= activeStored)
                                                {
                                                    active = ((maxActive - 1) < highDefault)? highDefault: (maxActive - 1);
                                                    pathFollowed = 5;
                                                }
                                            else
                                                {
                                                    active = maxActive;
                                                    pathFollowed = 6;
                                                }
                                        }
                                    stmt10.execute(true);
                                }
                            else if ( ratioSuccessFailure < 99)
                                {
                                    if(ratioSuccessFailure > rateStored && ratioSuccessFailure > 90 && retry <= retryStored)
                                        {
                                            active = maxActive;
                                            pathFollowed = 7;
                                        }
                                    else
                                        {
                                            active = ((maxActive - 2) < highDefault)? highDefault: (maxActive - 2);
                                            pathFollowed = 8;
                                        }
                                    stmt10.execute(true);
                                }
                            else
                                {
                                    active = maxActive;
                                    pathFollowed = 9;

                                    stmt10.execute(true);
                                }

                            updateOptimizerEvolution(sql, source_hostname, destin_hostname, active, throughput, ratioSuccessFailure, pathFollowed, bandwidthIn);

                            sql.commit();
                        }
                } //end for
        } //end try
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return allowed;
}



int MySqlAPI::getSeOut(const std::string & source, const std::set<std::string> & destination)
{
    soci::session sql(*connectionPool);

    // total number of allowed active for the source (both currently in use and free credits)
    int ret = 0;

    try
        {
            int nActiveSource=0;

            std::set<std::string>::iterator it;

            std::string source_hostname = source;

            sql << "SELECT COUNT(*) FROM t_file "
                "WHERE t_file.source_se = :source AND t_file.file_state in ('READY','ACTIVE')  ",
                soci::use(source_hostname), soci::into(nActiveSource);

            ret += nActiveSource;

            for (it = destination.begin(); it != destination.end(); ++it)
                {
                    std::string destin_hostname = *it;
                    ret += getCredits(sql, source_hostname, destin_hostname);
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

int MySqlAPI::getSeIn(const std::set<std::string> & source, const std::string & destination)
{
    soci::session sql(*connectionPool);

    // total number of allowed active for the source (both currently in use and free credits)
    int ret = 0;

    try
        {
            int nActiveDest=0;

            std::set<std::string>::iterator it;

            std::string destin_hostname = destination;

            sql << "SELECT COUNT(*) FROM t_file "
                "WHERE t_file.dest_se = :dst AND t_file.file_state in ('READY','ACTIVE') ",
                soci::use(destin_hostname), soci::into(nActiveDest);

            ret += nActiveDest;

            for (it = source.begin(); it != source.end(); ++it)
                {
                    std::string source_hostname = *it;
                    ret += getCredits(sql, source_hostname, destin_hostname);
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

int MySqlAPI::getCredits(soci::session& sql, const std::string & source_hostname, const std::string & destin_hostname)
{
    int freeCredits = 0;
    int limit = 0;
    int maxActive = 0;
    soci::indicator isNull = soci::i_ok;

    try
        {
            sql << " select count(*) from t_file where source_se=:source_se and dest_se=:dest_se and file_state in ('READY','ACTIVE') ",
                soci::use(source_hostname),
                soci::use(destin_hostname),
                soci::into(limit);

            sql << "select active from t_optimize_active where source_se=:source_se and dest_se=:dest_se",
                soci::use(source_hostname),
                soci::use(destin_hostname),
                soci::into(maxActive, isNull);

            if (isNull != soci::i_null)
                freeCredits = maxActive - limit;

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return freeCredits;
}



void MySqlAPI::setAllowedNoOptimize(const std::string & job_id, int file_id, const std::string & params)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            if (file_id)
                {
                    soci::statement stmt1 = (
                                                sql.prepare << "UPDATE t_file SET internal_file_params = :params WHERE file_id = :fileId AND job_id = :jobId",
                                                soci::use(params), soci::use(file_id), soci::use(job_id));
                    stmt1.execute(true);
                }
            else
                {
                    soci::statement stmt2 = (
                                                sql.prepare << "UPDATE t_file SET internal_file_params = :params WHERE job_id = :jobId",
                                                soci::use(params), soci::use(job_id));
                    stmt2.execute(true);
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



void MySqlAPI::forceFailTransfers(std::map<int, std::string>& collectJobs)
{
    soci::session sql(*connectionPool);

    try
        {
            std::string jobId, params, tHost,reuse;
            int fileId=0, pid=0, timeout=0;
            struct tm startTimeSt;
            time_t startTime;
            double diff = 0.0;
            soci::indicator isNull = soci::i_ok;
            soci::indicator isNullParams = soci::i_ok;
            soci::indicator isNullPid = soci::i_ok;
            int count = 0;

            soci::statement stmt = (
                                       sql.prepare <<
                                       " SELECT f.job_id, f.file_id, f.start_time, f.pid, f.internal_file_params, "
                                       " j.reuse_job "
                                       " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                       " WHERE f.file_state='ACTIVE' AND f.pid IS NOT NULL and f.job_finished is NULL "
                                       " AND (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) ",
                                       soci::use(hashSegment.start), soci::use(hashSegment.end),
                                       soci::into(jobId), soci::into(fileId), soci::into(startTimeSt),
                                       soci::into(pid, isNullPid), soci::into(params, isNullParams), soci::into(reuse, isNull)
                                   );

            soci::statement stmt1 = (
                                        sql.prepare <<
                                        " SELECT COUNT(*) FROM t_file WHERE job_id = :jobId ", soci::use(jobId), soci::into(count)
                                    );


            if (stmt.execute(true))
                {

                    do
                        {
                            startTime = timegm(&startTimeSt); //from db
                            time_t now2 = getUTC(0);

                            if (isNullParams != soci::i_null)
                                {
                                    timeout = extractTimeout(params);
                                    if(timeout == 0)
                                        timeout = 7200;
                                }
                            else
                                {
                                    timeout = 7200;
                                }

                            int terminateTime = timeout + 1000;

                            if (isNull != soci::i_null && reuse == "Y")
                                {
                                    count = 0;

                                    stmt1.execute(true);

                                    if(count > 0)
                                        terminateTime = count * 360;
                                }

                            diff = difftime(now2, startTime);
                            if (diff > terminateTime)
                                {
                                    if(isNullPid != soci::i_null && pid > 0)
                                        {
                                            FTS3_COMMON_LOGGER_NEWLOG(INFO) << "Killing pid:" << pid << ", jobid:" << jobId << ", fileid:" << fileId << " because it was stalled" << commit;
                                            kill(pid, SIGUSR1);
                                        }
                                    collectJobs.insert(std::make_pair(fileId, jobId));
                                    updateFileTransferStatusInternal(sql, 0.0, jobId, fileId,
                                                                     "FAILED", "Transfer has been forced-killed because it was stalled",
                                                                     pid, 0, 0, false);
                                    updateJobTransferStatusInternal(sql, jobId, "FAILED",0);
                                }

                        }
                    while (stmt.fetch());
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




void MySqlAPI::setAllowed(const std::string & job_id, int file_id, const std::string & /*source_se*/, const std::string & /*dest*/,
                          int nostreams, int timeout, int buffersize)
{
    soci::session sql(*connectionPool);
    std::stringstream params;

    try
        {
            sql.begin();

            params << "nostreams:" << nostreams << ",timeout:" << timeout << ",buffersize:" << buffersize;

            if (file_id != -1)
                {
                    sql << "UPDATE t_file SET internal_file_params = :params WHERE file_id = :fileId AND job_id = :jobId",
                        soci::use(params.str()), soci::use(file_id), soci::use(job_id);

                }
            else
                {

                    sql << "UPDATE t_file SET internal_file_params = :params WHERE job_id = :jobId",
                        soci::use(params.str()), soci::use(job_id);
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



bool MySqlAPI::terminateReuseProcess(const std::string & jobId, int pid, const std::string & message)
{
    bool ok = true;
    soci::session sql(*connectionPool);
    std::string job_id;
    std::string reuse;
    soci::indicator reuseInd = soci::i_ok;

    try
        {
            if(jobId.length() == 0)
                {
                    sql << " SELECT job_id from t_file where pid=:pid and job_finished is NULL LIMIT 1",
                        soci::use(pid), soci::into(job_id);

                    sql << " SELECT reuse_job FROM t_job WHERE job_id = :jobId AND reuse_job IS NOT NULL",
                        soci::use(job_id), soci::into(reuse, reuseInd);

                    if (sql.got_data() && reuse == "Y")
                        {
                            sql.begin();
                            sql << " UPDATE t_file SET file_state = 'FAILED', job_finished=UTC_TIMESTAMP(), finish_time=UTC_TIMESTAMP(), "
                                " reason=:message WHERE (job_id = :jobId OR pid=:pid) AND file_state not in ('FINISHED','FAILED','CANCELED') ",
                                soci::use(message),
                                soci::use(job_id),
                                soci::use(pid);
                            sql.commit();
                        }

                }
            else
                {
                    sql << " SELECT reuse_job FROM t_job WHERE job_id = :jobId AND reuse_job IS NOT NULL",
                        soci::use(jobId), soci::into(reuse, reuseInd);

                    if (sql.got_data() && reuse == "Y")
                        {
                            sql.begin();
                            sql << " UPDATE t_file SET file_state = 'FAILED', job_finished=UTC_TIMESTAMP(), finish_time=UTC_TIMESTAMP(), "
                                " reason=:message WHERE (job_id = :jobId OR pid=:pid) AND file_state not in ('FINISHED','FAILED','CANCELED') ",
                                soci::use(message),
                                soci::use(job_id),
                                soci::use(pid);
                            sql.commit();
                        }
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            return ok;
        }
    catch (...)
        {
            sql.rollback();
            return ok;
        }
    return ok;
}



void MySqlAPI::setPid(const std::string & /*jobId*/, int fileId, int pid)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            soci::statement stmt1 = (
                                        sql.prepare << "UPDATE t_file SET pid = :pid WHERE file_id = :fileId",
                                        soci::use(pid), soci::use(fileId));
            stmt1.execute(true);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::setPidV(int pid, std::map<int, std::string>& pids)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_file SET pid = :pid WHERE job_id = :jobId ", soci::use(pid), soci::use(pids.begin()->second);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::revertToSubmitted()
{
    soci::session sql(*connectionPool);

    try
        {
            struct tm startTime;
            int fileId=0;
            std::string jobId, reuseJob;
            time_t now2 = getUTC(0);
            int count = 0;
            int terminateTime = 0;


            soci::indicator reuseInd = soci::i_ok;
            soci::statement readyStmt = (sql.prepare << "SELECT f.start_time, f.file_id, f.job_id, j.reuse_job "
                                         " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                         " WHERE f.file_state = 'READY' and j.job_finished is null "
                                         " AND (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd) ",
                                         soci::use(hashSegment.start), soci::use(hashSegment.end),
                                         soci::into(startTime),
                                         soci::into(fileId),
                                         soci::into(jobId),
                                         soci::into(reuseJob, reuseInd));

            soci::statement readyStmt1 = (sql.prepare << "UPDATE t_file SET start_time=NULL, file_state = 'SUBMITTED', reason='', transferhost='',finish_time=NULL, job_finished=NULL "
                                          "WHERE file_id = :fileId AND job_id= :jobId AND file_state = 'READY' ",
                                          soci::use(fileId),soci::use(jobId));

            soci::statement readyStmt2 = (sql.prepare << " SELECT COUNT(*) FROM t_file WHERE job_id = :jobId ", soci::use(jobId), soci::into(count));

            soci::statement readyStmt3 = (sql.prepare << "UPDATE t_job SET job_state = 'SUBMITTED' where job_id = :jobId ", soci::use(jobId));

            soci::statement readyStmt4 = (sql.prepare << "UPDATE t_file SET start_time=NULL, file_state = 'SUBMITTED', reason='', transferhost='',finish_time=NULL, job_finished=NULL "
                                          "WHERE file_state = 'READY' AND "
                                          "      job_finished IS NULL AND file_id = :fileId",
                                          soci::use(fileId));


            sql.begin();
            if (readyStmt.execute(true))
                {
                    do
                        {
                            time_t startTimestamp = timegm(&startTime);
                            double diff = difftime(now2, startTimestamp);

                            if (diff > 500 && reuseJob != "Y")
                                {
                                    FTS3_COMMON_LOGGER_NEWLOG(ERR) << "The transfer with file id " << fileId << " seems to be stalled, restart it" << commit;

                                    readyStmt1.execute(true);
                                }
                            else
                                {
                                    if(reuseJob == "Y")
                                        {
                                            count = 0;
                                            terminateTime = 0;

                                            readyStmt2.execute(true);

                                            if(count > 0)
                                                terminateTime = count * 1000;

                                            if(diff > terminateTime)
                                                {
                                                    FTS3_COMMON_LOGGER_NEWLOG(INFO) << "The transfer with file id (reuse) " << fileId << " seems to be stalled, restart it" << commit;

                                                    readyStmt3.execute(true);

                                                    readyStmt4.execute(true);
                                                }
                                        }
                                }
                        }
                    while (readyStmt.fetch());
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




void MySqlAPI::backup(long* nJobs, long* nFiles)
{
    try
        {
            unsigned index=0, count=0, start=0, end=0;
            std::string service_name = "fts_backup";
            updateHeartBeat(&index, &count, &start, &end, service_name);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    soci::session sql(*connectionPool);

    *nJobs = 0;
    *nFiles = 0;
    std::ostringstream jobIdStmt;
    std::string job_id;
    std::string stmt;

    try
        {
            //prevent more than on server to update the optimizer decisions
            if(hashSegment.start == 0)
                {
                    soci::rowset<soci::row> rs = (
                                                     sql.prepare <<
                                                     "  select  job_id from t_job where job_finished < (UTC_TIMESTAMP() - interval '7' DAY ) "
                                                 );

                    int count = 0;
                    bool drain = false;

                    for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                        {
                            drain = getDrainInternal(sql);
                            if(drain)
                                {
                                    sql.commit();
                                    return;
                                }

                            count++;
                            soci::row const& r = *i;
                            job_id = r.get<std::string>("job_id");
                            jobIdStmt << "'";
                            jobIdStmt << job_id;
                            jobIdStmt << "',";

                            if(count == 1000)
                                {
                                    std::string queryStr = jobIdStmt.str();
                                    job_id = queryStr.substr(0, queryStr.length() - 1);

                                    sql.begin();
                                    stmt = "DELETE FROM t_file WHERE job_id in (" +job_id+ ")";
                                    sql << stmt;
                                    stmt = "DELETE FROM t_job WHERE job_id in (" +job_id+ ")";
                                    sql << stmt;

                                    stmt = "INSERT INTO t_job_backup SELECT * FROM t_job WHERE job_id  in (" +job_id+ ")";
                                    sql << stmt;
                                    stmt = "INSERT INTO t_file_backup SELECT * FROM t_file WHERE  job_id  in (" +job_id+ ")";
                                    sql << stmt;

                                    count = 0;
                                    jobIdStmt.str(std::string());
                                    jobIdStmt.clear();
                                    sql.commit();
                                }
                        }

                    //delete from t_optimizer_evolution > 7 days old records
                    sql.begin();
                    sql << "delete from t_optimizer_evolution where datetime < (UTC_TIMESTAMP() - interval '7' DAY )";
                    sql.commit();

                    //delete from t_file_retry_errors > 7 days old records
                    sql.begin();
                    sql << "delete from t_file_retry_errors where datetime < (UTC_TIMESTAMP() - interval '7' DAY )";
                    sql.commit();
		    
                    //delete from t_turl > 7 days old records
                    sql.begin();
                    sql << "delete from t_turl where datetime < (UTC_TIMESTAMP() - interval '7' DAY )";
		    sql.commit();
		    
		    sql.begin();
		    sql << "update t_turl set finish=0 where finish > 100000000000";
		    sql << "update t_turl set fail=0 where fail > 100000000000";
                    sql.commit();		    
                }

            jobIdStmt.str(std::string());
            jobIdStmt.clear();
        }
    catch (std::exception& e)
        {
            jobIdStmt.str(std::string());
            jobIdStmt.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            jobIdStmt.str(std::string());
            jobIdStmt.clear();
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}




void MySqlAPI::forkFailedRevertState(const std::string & jobId, int fileId)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "UPDATE t_file SET file_state = 'SUBMITTED', transferhost='', start_time=NULL "
                "WHERE file_id = :fileId AND job_id = :jobId AND  transferhost= :hostname AND "
                "      file_state NOT IN ('FINISHED','FAILED','CANCELED')",
                soci::use(fileId), soci::use(jobId), soci::use(hostname);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::forkFailedRevertStateV(std::map<int, std::string>& pids)
{
    soci::session sql(*connectionPool);

    try
        {
            int fileId=0;
            std::string jobId;


            soci::statement stmt = (sql.prepare << "UPDATE t_file SET file_state = 'SUBMITTED', transferhost='', start_time=NULL "
                                    "WHERE file_id = :fileId AND job_id = :jobId AND "
                                    "      file_state NOT IN ('FINISHED','FAILED','CANCELED')",
                                    soci::use(fileId), soci::use(jobId));


            sql.begin();
            for (std::map<int, std::string>::const_iterator i = pids.begin(); i != pids.end(); ++i)
                {
                    fileId = i->first;
                    jobId  = i->second;
                    stmt.execute(true);
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



bool MySqlAPI::retryFromDead(std::vector<struct message_updater>& messages, bool diskFull)
{
    soci::session sql(*connectionPool);

    bool ok = false;
    std::vector<struct message_updater>::const_iterator iter;
    const std::string transfer_status = "FAILED";
    std::string transfer_message;
    if(diskFull)
        {
            transfer_message = "No space left on device in " + hostname;
        }
    else
        {
            transfer_message = "no FTS server had updated the transfer status the last 600 seconds, probably stalled in " + hostname;
        }

    const std::string status = "FAILED";

    try
        {
            for (iter = messages.begin(); iter != messages.end(); ++iter)
                {

                    soci::rowset<int> rs = (
                                               sql.prepare <<
                                               " SELECT file_id FROM t_file "
                                               " WHERE file_id = :fileId AND job_id = :jobId AND file_state='ACTIVE' AND"
                                               " (hashed_id >= :hStart AND hashed_id <= :hEnd) ",
                                               soci::use(iter->file_id),
                                               soci::use(std::string(iter->job_id)),
                                               soci::use(hashSegment.start),
                                               soci::use(hashSegment.end));

                    if (rs.begin() != rs.end())
                        {
                            ok = true;
                            updateFileTransferStatusInternal(sql, 0.0, (*iter).job_id, (*iter).file_id, transfer_status, transfer_message, (*iter).process_id, 0, 0,false);
                            updateJobTransferStatusInternal(sql, (*iter).job_id, status,0);
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return ok;
}



void MySqlAPI::blacklistSe(std::string se, std::string vo, std::string status, int timeout, std::string msg, std::string adm_dn)
{
    soci::session sql(*connectionPool);

    try
        {

            int count = 0;

            sql <<
                " SELECT COUNT(*) FROM t_bad_ses WHERE se = :se ",
                soci::use(se),
                soci::into(count)
                ;

            sql.begin();

            if (count)
                {

                    sql << " UPDATE t_bad_ses SET "
                        "	addition_time = UTC_TIMESTAMP(), "
                        "	admin_dn = :admin, "
                        "  	vo = :vo, "
                        "	status = :status, "
                        "  	wait_timeout = :timeout "
                        " WHERE se = :se ",
                        soci::use(adm_dn),
                        soci::use(vo),
                        soci::use(status),
                        soci::use(timeout),
                        soci::use(se)
                        ;

                }
            else
                {

                    sql << "INSERT INTO t_bad_ses (se, message, addition_time, admin_dn, vo, status, wait_timeout) "
                        "               VALUES (:se, :message, UTC_TIMESTAMP(), :admin, :vo, :status, :timeout)",
                        soci::use(se), soci::use(msg), soci::use(adm_dn), soci::use(vo), soci::use(status), soci::use(timeout);
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::blacklistDn(std::string dn, std::string msg, std::string adm_dn)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql << "INSERT IGNORE INTO t_bad_dns (dn, message, addition_time, admin_dn) "
                "               VALUES (:dn, :message, UTC_TIMESTAMP(), :admin)",
                soci::use(dn), soci::use(msg), soci::use(adm_dn);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::unblacklistSe(std::string se)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            // delete the entry from DB
            sql << "DELETE FROM t_bad_ses WHERE se = :se",
                soci::use(se)
                ;
            // set to null pending fields in respective files
            sql <<
                " UPDATE t_file f, t_job j SET f.wait_timestamp = NULL, f.wait_timeout = NULL "
                " WHERE f.job_id = j.job_id AND (f.source_se = :src OR f.dest_se = :dest) "
                "	AND f.file_state IN ('ACTIVE', 'READY', 'SUBMITTED') "
                "	AND NOT EXISTS ( "
                "		SELECT NULL "
                "		FROM t_bad_dns "
                "		WHERE dn = j.user_dn AND (status = 'WAIT' OR status = 'WAIT_AS')"
                "	)",
                soci::use(se),
                soci::use(se)
                ;

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::unblacklistDn(std::string dn)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            // delete the entry from DB
            sql << "DELETE FROM t_bad_dns WHERE dn = :dn",
                soci::use(dn)
                ;
            // set to null pending fields in respective files
            sql <<
                " UPDATE t_file f, t_job j SET f.wait_timestamp = NULL, f.wait_timeout = NULL "
                " WHERE f.job_id = j.job_id AND j.user_dn = :dn "
                "	AND f.file_state IN ('ACTIVE', 'READY', 'SUBMITTED') "
                "	AND NOT EXISTS ( "
                "		SELECT NULL "
                "		FROM t_bad_ses "
                "		WHERE (se = f.source_se OR se = f.dest_se) AND status = 'WAIT' "
                "	)",
                soci::use(dn)
                ;

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



bool MySqlAPI::isSeBlacklisted(std::string se, std::string vo)
{
    soci::session sql(*connectionPool);

    bool blacklisted = false;
    try
        {
            sql << "SELECT se FROM t_bad_ses WHERE se = :se AND (vo IS NULL OR vo='' OR vo = :vo)", soci::use(se), soci::use(vo);
            blacklisted = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return blacklisted;
}


bool MySqlAPI::allowSubmitForBlacklistedSe(std::string se)
{
    soci::session sql(*connectionPool);

    bool ret = false;
    try
        {
            sql << "SELECT se FROM t_bad_ses WHERE se = :se AND status = 'WAIT_AS'", soci::use(se);
            ret = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return ret;
}

void MySqlAPI::allowSubmit(std::string ses, std::string vo, std::list<std::string>& notAllowed)
{
    soci::session sql(*connectionPool);

    std::string query = "SELECT se FROM t_bad_ses WHERE se IN " + ses + " AND status != 'WAIT_AS' AND (vo IS NULL OR vo='' OR vo = :vo)";

    try
        {
            soci::rowset<std::string> rs = (sql.prepare << query, soci::use(vo));

            for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    notAllowed.push_back(*i);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

boost::optional<int> MySqlAPI::getTimeoutForSe(std::string se)
{
    soci::session sql(*connectionPool);

    boost::optional<int> ret;

    try
        {
            soci::indicator isNull = soci::i_ok;
            int tmp = 0;

            sql <<
                " SELECT wait_timeout FROM t_bad_ses WHERE se = :se ",
                soci::use(se),
                soci::into(tmp, isNull)
                ;

            if (sql.got_data())
                {
                    if (isNull == soci::i_ok) ret = tmp;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

void MySqlAPI::getTimeoutForSe(std::string ses, std::map<std::string, int>& ret)
{
    soci::session sql(*connectionPool);

    std::string query =
        " select se, wait_timeout  "
        " from t_bad_ses "
        " where se in "
        ;
    query += ses;

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare << query
                                         );

            for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    ret[i->get<std::string>("se")] =  i->get<int>("wait_timeout");
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


bool MySqlAPI::isDnBlacklisted(std::string dn)
{
    soci::session sql(*connectionPool);

    bool blacklisted = false;
    try
        {
            sql << "SELECT dn FROM t_bad_dns WHERE dn = :dn", soci::use(dn);
            blacklisted = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return blacklisted;
}



bool MySqlAPI::isFileReadyState(int fileID)
{
    soci::session sql(*connectionPool);
    bool isReadyState = false;
    bool isReadyHost = false;
    std::string host;
    std::string state;
    soci::indicator isNull = soci::i_ok;
    std::string vo_name;
    std::string dest_se;
    std::string dest_surl;
    long long countSame = 0;

    try
        {
            sql << "SELECT file_state, transferHost, dest_surl, vo_name, dest_se FROM t_file WHERE file_id = :fileId",
                soci::use(fileID),
                soci::into(state),
                soci::into(host, isNull),
                soci::into(dest_surl),
                soci::into(vo_name),
                soci::into(dest_se);

            isReadyState = (state == "READY");

            if (isNull != soci::i_null)
                isReadyHost = (host == hostname);

            sql << "select count(*) from t_file where file_state in ('READY','ACTIVE') and dest_surl=:dest_surl and vo_name=:vo_name and dest_se=:dest_se ",
                soci::use(dest_surl),
                soci::use(vo_name),
                soci::use(dest_se),
                soci::into(countSame);

            if(countSame > 1)
                {
                    return false;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return (isReadyState && isReadyHost);
}



bool MySqlAPI::checkGroupExists(const std::string & groupName)
{
    soci::session sql(*connectionPool);

    bool exists = false;
    try
        {
            std::string grp;
            sql << "SELECT groupName FROM t_group_members WHERE groupName = :group",
                soci::use(groupName), soci::into(grp);

            exists = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return exists;
}

//t_group_members

void MySqlAPI::getGroupMembers(const std::string & groupName, std::vector<std::string>& groupMembers)
{
    soci::session sql(*connectionPool);

    try
        {
            soci::rowset<std::string> rs = (sql.prepare << "SELECT member FROM t_group_members "
                                            "WHERE groupName = :group",
                                            soci::use(groupName));
            for (soci::rowset<std::string>::const_iterator i = rs.begin();
                    i != rs.end(); ++i)
                groupMembers.push_back(*i);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



std::string MySqlAPI::getGroupForSe(const std::string se)
{
    soci::session sql(*connectionPool);

    std::string group;
    try
        {
            sql << "SELECT groupName FROM t_group_members "
                "WHERE member = :member",
                soci::use(se), soci::into(group);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return group;
}



void MySqlAPI::addMemberToGroup(const std::string & groupName, std::vector<std::string>& groupMembers)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            std::string member;
            soci::statement stmt = (sql.prepare << "INSERT INTO t_group_members(member, groupName) "
                                    "                    VALUES (:member, :group)",
                                    soci::use(member), soci::use(groupName));
            for (std::vector<std::string>::const_iterator i = groupMembers.begin();
                    i != groupMembers.end(); ++i)
                {
                    member = *i;
                    stmt.execute(true);
                }


            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::deleteMembersFromGroup(const std::string & groupName, std::vector<std::string>& groupMembers)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            std::string member;
            soci::statement stmt = (sql.prepare << "DELETE FROM t_group_members "
                                    "WHERE groupName = :group AND member = :member",
                                    soci::use(groupName), soci::use(member));
            for (std::vector<std::string>::const_iterator i = groupMembers.begin();
                    i != groupMembers.end(); ++i)
                {
                    member = *i;
                    stmt.execute(true);
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::addLinkConfig(LinkConfig* cfg)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "INSERT INTO t_link_config (source, destination, state, symbolicName, "
                "                          nostreams, tcp_buffer_size, urlcopy_tx_to, no_tx_activity_to, auto_tuning)"
                "                  VALUES (:src, :dest, :state, :sname, :nstreams, :tcp, :txto, :txactivity, :auto_tuning)",
                soci::use(cfg->source), soci::use(cfg->destination),
                soci::use(cfg->state), soci::use(cfg->symbolic_name),
                soci::use(cfg->NOSTREAMS), soci::use(cfg->TCP_BUFFER_SIZE),
                soci::use(cfg->URLCOPY_TX_TO), soci::use(cfg->URLCOPY_TX_TO),
                soci::use(cfg->auto_tuning);


            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::updateLinkConfig(LinkConfig* cfg)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_link_config SET "
                "  state = :state, symbolicName = :sname, "
                "  nostreams = :nostreams, tcp_buffer_size = :tcp, "
                "  urlcopy_tx_to = :txto, no_tx_activity_to = :txactivity, auto_tuning = :auto_tuning "
                "WHERE source = :source AND destination = :dest",
                soci::use(cfg->state), soci::use(cfg->symbolic_name),
                soci::use(cfg->NOSTREAMS), soci::use(cfg->TCP_BUFFER_SIZE),
                soci::use(cfg->URLCOPY_TX_TO), soci::use(cfg->NO_TX_ACTIVITY_TO),
                soci::use(cfg->auto_tuning),
                soci::use(cfg->source), soci::use(cfg->destination);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::deleteLinkConfig(std::string source, std::string destination)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "DELETE FROM t_link_config WHERE source = :source AND destination = :destination",
                soci::use(source), soci::use(destination);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



LinkConfig* MySqlAPI::getLinkConfig(std::string source, std::string destination)
{
    soci::session sql(*connectionPool);

    LinkConfig* lnk = NULL;
    try
        {
            LinkConfig config;

            sql << "SELECT * FROM t_link_config WHERE source = :source AND destination = :dest",
                soci::use(source), soci::use(destination),
                soci::into(config);

            if (sql.got_data())
                lnk = new LinkConfig(config);
        }
    catch (std::exception& e)
        {
            if(lnk)
                delete lnk;
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            if(lnk)
                delete lnk;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return lnk;
}



std::pair<std::string, std::string>* MySqlAPI::getSourceAndDestination(std::string symbolic_name)
{
    soci::session sql(*connectionPool);

    std::pair<std::string, std::string>* pair = NULL;
    try
        {
            std::string source, destination;
            sql << "SELECT source, destination FROM t_link_config WHERE symbolicName = :sname",
                soci::use(symbolic_name), soci::into(source), soci::into(destination);
            if (sql.got_data())
                pair = new std::pair<std::string, std::string>(source, destination);
        }
    catch (std::exception& e)
        {
            if(pair)
                delete pair;
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            if(pair)
                delete pair;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return pair;
}



bool MySqlAPI::isGrInPair(std::string group)
{
    soci::session sql(*connectionPool);

    bool inPair = false;
    try
        {
            sql << "SELECT symbolicName FROM t_link_config WHERE "
                "  ((source = :group AND destination <> '*') OR "
                "  (source <> '*' AND destination = :group))",
                soci::use(group, "group");
            inPair = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return inPair;
}



void MySqlAPI::addShareConfig(ShareConfig* cfg)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "INSERT IGNORE INTO t_share_config (source, destination, vo, active) "
                "                    VALUES (:source, :destination, :vo, :active)",
                soci::use(cfg->source), soci::use(cfg->destination), soci::use(cfg->vo),
                soci::use(cfg->active_transfers);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::updateShareConfig(ShareConfig* cfg)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_share_config SET "
                "  active = :active "
                "WHERE source = :source AND destination = :dest AND vo = :vo",
                soci::use(cfg->active_transfers),
                soci::use(cfg->source), soci::use(cfg->destination), soci::use(cfg->vo);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::deleteShareConfig(std::string source, std::string destination, std::string vo)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "DELETE FROM t_share_config WHERE source = :source AND destination = :dest AND vo = :vo",
                soci::use(source), soci::use(destination), soci::use(vo);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::deleteShareConfig(std::string source, std::string destination)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "DELETE FROM t_share_config WHERE source = :source AND destination = :dest",
                soci::use(destination), soci::use(source);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



ShareConfig* MySqlAPI::getShareConfig(std::string source, std::string destination, std::string vo)
{
    soci::session sql(*connectionPool);

    ShareConfig* cfg = NULL;
    try
        {
            ShareConfig config;
            sql << "SELECT * FROM t_share_config WHERE "
                "  source = :source AND destination = :dest AND vo = :vo",
                soci::use(source), soci::use(destination), soci::use(vo),
                soci::into(config);
            if (sql.got_data())
                cfg = new ShareConfig(config);
        }
    catch (std::exception& e)
        {
            if(cfg)
                delete cfg;
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            if(cfg)
                delete cfg;
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return cfg;
}



std::vector<ShareConfig*> MySqlAPI::getShareConfig(std::string source, std::string destination)
{
    soci::session sql(*connectionPool);

    std::vector<ShareConfig*> cfg;
    try
        {
            soci::rowset<ShareConfig> rs = (sql.prepare << "SELECT * FROM t_share_config WHERE "
                                            "  source = :source AND destination = :dest",
                                            soci::use(source), soci::use(destination));
            for (soci::rowset<ShareConfig>::const_iterator i = rs.begin();
                    i != rs.end(); ++i)
                {
                    ShareConfig* newCfg = new ShareConfig(*i);
                    cfg.push_back(newCfg);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return cfg;
}


void MySqlAPI::addActivityConfig(std::string vo, std::string shares, bool active)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            const std::string act = active ? "on" : "off";

            sql << "INSERT INTO t_activity_share_config (vo, activity_share, active) "
                "                    VALUES (:vo, :share, :active)",
                soci::use(vo),
                soci::use(shares),
                soci::use(act)
                ;

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::updateActivityConfig(std::string vo, std::string shares, bool active)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            const std::string act = active ? "on" : "off";

            sql <<
                " UPDATE t_activity_share_config "
                " SET activity_share = :share, active = :active "
                " WHERE vo = :vo",
                soci::use(shares),
                soci::use(act),
                soci::use(vo)
                ;

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::deleteActivityConfig(std::string vo)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "DELETE FROM t_activity_share_config WHERE vo = :vo ",
                soci::use(vo);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

bool MySqlAPI::isActivityConfigActive(std::string vo)
{
    soci::session sql(*connectionPool);

    std::string active;
    soci::indicator isNull = soci::i_ok;

    try
        {
            sql << "SELECT active FROM t_activity_share_config WHERE vo = :vo ",
                soci::use(vo),
                soci::into(active, isNull)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return active == "on";
}

std::map< std::string, double > MySqlAPI::getActivityConfig(std::string vo)
{
    soci::session sql(*connectionPool);
    try
        {
            return getActivityShareConf(sql, vo);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


/*for session reuse check only*/
bool MySqlAPI::isFileReadyStateV(std::map<int, std::string>& fileIds)
{
    soci::session sql(*connectionPool);

    bool isReady = false;
    try
        {
            std::string state;
            sql << "SELECT file_state FROM t_file WHERE file_id = :fileId",
                soci::use(fileIds.begin()->first), soci::into(state);

            isReady = (state == "READY");
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return isReady;
}


/*
    we need to check if a member already belongs to another group
    true: it is member of another group
    false: it is not member of another group
 */
bool MySqlAPI::checkIfSeIsMemberOfAnotherGroup(const std::string & member)
{
    soci::session sql(*connectionPool);

    bool isMember = false;
    try
        {
            std::string group;
            sql << "SELECT groupName FROM t_group_members WHERE member = :member",
                soci::use(member), soci::into(group);

            isMember = sql.got_data();
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return isMember;
}



void MySqlAPI::addFileShareConfig(int file_id, std::string source, std::string destination, std::string vo)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << " insert ignore into t_file_share_config (file_id, source, destination, vo) "
                "  values(:file_id, :source, :destination, :vo)",
                soci::use(file_id),
                soci::use(source),
                soci::use(destination),
                soci::use(vo);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



int MySqlAPI::countActiveTransfers(std::string source, std::string destination, std::string vo)
{
    soci::session sql(*connectionPool);

    int nActive = 0;
    try
        {
            sql << "SELECT COUNT(*) FROM t_file, t_file_share_config "
                "WHERE t_file.file_state in ('ACTIVE','READY')  AND "
                "      t_file_share_config.file_id = t_file.file_id AND "
                "      t_file_share_config.source = :source AND "
                "      t_file_share_config.destination = :dest AND "
                "      t_file_share_config.vo = :vo",
                soci::use(source), soci::use(destination), soci::use(vo),
                soci::into(nActive);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return nActive;
}



int MySqlAPI::countActiveOutboundTransfersUsingDefaultCfg(std::string se, std::string vo)
{
    soci::session sql(*connectionPool);

    int nActiveOutbound = 0;
    try
        {
            sql << "SELECT COUNT(*) FROM t_file, t_file_share_config "
                "WHERE t_file.file_state in ('ACTIVE','READY')  AND "
                "      t_file.source_se = :source AND "
                "      t_file.file_id = t_file_share_config.file_id AND "
                "      t_file_share_config.source = '(*)' AND "
                "      t_file_share_config.destination = '*' AND "
                "      t_file_share_config.vo = :vo",
                soci::use(se), soci::use(vo), soci::into(nActiveOutbound);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return nActiveOutbound;
}



int MySqlAPI::countActiveInboundTransfersUsingDefaultCfg(std::string se, std::string vo)
{
    soci::session sql(*connectionPool);

    int nActiveInbound = 0;
    try
        {
            sql << "SELECT COUNT(*) FROM t_file, t_file_share_config "
                "WHERE t_file.file_state in ('ACTIVE','READY') AND "
                "      t_file.dest_se = :dest AND "
                "      t_file.file_id = t_file_share_config.file_id AND "
                "      t_file_share_config.source = '*' AND "
                "      t_file_share_config.destination = '(*)' AND "
                "      t_file_share_config.vo = :vo",
                soci::use(se), soci::use(vo), soci::into(nActiveInbound);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return nActiveInbound;
}

int MySqlAPI::sumUpVoShares (std::string source, std::string destination, std::set<std::string> vos)
{
    soci::session sql(*connectionPool);

    int sum = 0;
    try
        {

            std::set<std::string>::iterator it = vos.begin();

            while (it != vos.end())
                {

                    std::set<std::string>::iterator remove = it;
                    it++;

                    sql <<
                        " SELECT vo "
                        " FROM t_share_config "
                        " WHERE source = :source "
                        "	AND destination = :destination "
                        "	AND vo = :vo ",
                        soci::use(source),
                        soci::use(destination),
                        soci::use(*remove)
                        ;

                    if (!sql.got_data() && *remove != "public")
                        {
                            // if there is no configuration for this VO replace it with 'public'
                            vos.erase(remove);
                            vos.insert("public");
                        }
                }

            std::string vos_str = "(";

            for (it = vos.begin(); it != vos.end(); ++it)
                {

                    vos_str += "'" + *it + "'" + ",";
                }

            // replace the last ',' with closing ')'
            vos_str[vos_str.size() - 1] = ')';

            soci::indicator isNull = soci::i_ok;

            sql <<
                " SELECT SUM(active) "
                " FROM t_share_config "
                " WHERE source = :source "
                "	AND destination = :destination "
                "	AND vo IN " + vos_str,
                soci::use(source),
                soci::use(destination),
                soci::into(sum, isNull)
                ;

            if (isNull == soci::i_null) sum = 0;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return sum;
}


void MySqlAPI::setPriority(std::string job_id, int priority)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_job SET "
                "  priority = :priority "
                "WHERE job_id = :jobId",
                soci::use(priority), soci::use(job_id);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::setSeProtocol(std::string /*protocol*/, std::string se, std::string state)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            int count = 0;

            sql <<
                " SELECT count(auto_number) "
                " FROM t_optimize "
                " WHERE source_se = :se ",
                soci::use(se), soci::into(count)
                ;

            if (count)
                {
                    sql <<
                        " UPDATE t_optimize "
                        " SET udt = :udt "
                        " WHERE source_se = :se ",
                        soci::use(state),
                        soci::use(se)
                        ;
                }
            else
                {
                    sql <<
                        " INSERT INTO t_optimize (source_se, udt, file_id) "
                        " VALUES (:se, :udt, 0)",
                        soci::use(se),
                        soci::use(state)
                        ;

                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::setRetry(int retry)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_server_config SET retry = :retry",
                soci::use(retry);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



int MySqlAPI::getRetry(const std::string & jobId)
{
    soci::session sql(*connectionPool);

    int nRetries = 0;
    soci::indicator isNull = soci::i_ok;

    try
        {

            sql <<
                " SELECT retry "
                " FROM t_job "
                " WHERE job_id = :jobId ",
                soci::use(jobId),
                soci::into(nRetries, isNull)
                ;

            if (isNull != soci::i_null && nRetries == 0)
                {
                    sql <<
                        " SELECT retry "
                        " FROM t_server_config LIMIT 1",
                        soci::into(nRetries)
                        ;
                }
            else if (isNull != soci::i_null && nRetries < 0)
                {
                    nRetries = 0;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return nRetries;
}



int MySqlAPI::getRetryTimes(const std::string & jobId, int fileId)
{
    soci::session sql(*connectionPool);

    int nRetries = 0;
    soci::indicator isNull = soci::i_ok;

    try
        {
            sql << "SELECT retry FROM t_file WHERE file_id = :fileId AND job_id = :jobId ",
                soci::use(fileId), soci::use(jobId), soci::into(nRetries, isNull);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return nRetries;
}



int MySqlAPI::getMaxTimeInQueue()
{
    soci::session sql(*connectionPool);

    int maxTime = 0;
    try
        {
            soci::indicator isNull = soci::i_ok;

            sql << "SELECT max_time_queue FROM t_server_config LIMIT 1",
                soci::into(maxTime, isNull);

            //just in case soci it is reseting the value to NULL
            if(isNull != soci::i_null && maxTime > 0)
                return maxTime;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return maxTime;
}



void MySqlAPI::setMaxTimeInQueue(int afterXHours)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_server_config SET max_time_queue = :maxTime",
                soci::use(afterXHours);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::setToFailOldQueuedJobs(std::vector<std::string>& jobs)
{
    const static std::string message = "Job has been canceled because it stayed in the queue for too long";

    int maxTime = 0;

    try
        {
            maxTime = getMaxTimeInQueue();
            if (maxTime == 0)
                return;
        }
    catch (std::exception& ex)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + ex.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    // Acquire the session after calling getMaxTimeInQueue to avoid
    // deadlocks (sql acquired and getMaxTimeInQueue locked
    // waiting for a session we have)
    soci::session sql(*connectionPool);

    try
        {
            if(hashSegment.start == 0)
                {
                    std::string job_id;
                    soci::rowset<std::string> rs = (sql.prepare << "SELECT job_id FROM t_job WHERE "
                                                    "    submit_time < (UTC_TIMESTAMP() - interval :interval hour) AND "
                                                    "    job_state in ('SUBMITTED', 'READY') and job_finished is NULL ",
                                                    soci::use(maxTime));

                    soci::statement stmt1 = (sql.prepare << "UPDATE t_file SET job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(),  file_state = 'CANCELED', reason = :reason WHERE job_id = :jobId AND file_state IN ('SUBMITTED','READY')",
                                             soci::use(message), soci::use(job_id));


                    soci::statement stmt2 = ( sql.prepare << "UPDATE t_job SET job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), job_state = 'CANCELED', reason = :reason WHERE job_id = :jobId AND job_state IN ('SUBMITTED','READY')",
                                              soci::use(message), soci::use(job_id));



                    sql.begin();
                    for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                        {
                            job_id = (*i);

                            stmt1.execute(true);
                            stmt2.execute(true);

                            jobs.push_back(*i);
                        }
                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



std::vector< std::pair<std::string, std::string> > MySqlAPI::getPairsForSe(std::string se)
{
    soci::session sql(*connectionPool);

    std::vector< std::pair<std::string, std::string> > ret;

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " select source, destination "
                                             " from t_link_config "
                                             " where (source = :source and destination <> '*') "
                                             "	or (source <> '*' and destination = :dest) ",
                                             soci::use(se),
                                             soci::use(se)
                                         );

            for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    ret.push_back(
                        make_pair(i->get<std::string>("source"), i->get<std::string>("destination"))
                    );
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}


std::vector<std::string> MySqlAPI::getAllStandAlloneCfgs()
{

    soci::session sql(*connectionPool);

    std::vector<std::string> ret;

    try
        {
            soci::rowset<std::string> rs = (
                                               sql.prepare <<
                                               " select source "
                                               " from t_link_config "
                                               " where destination = '*' and auto_tuning <> 'all' "
                                           );

            for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    ret.push_back(*i);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

std::vector< std::pair<std::string, std::string> > MySqlAPI::getAllPairCfgs()
{

    soci::session sql(*connectionPool);

    std::vector< std::pair<std::string, std::string> > ret;

    try
        {
            soci::rowset<soci::row> rs = (sql.prepare << "select source, destination from t_link_config where source <> '*' and destination <> '*'");

            for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    soci::row const& row = *i;
                    ret.push_back(
                        std::make_pair(row.get<std::string>("source"), row.get<std::string>("destination"))
                    );
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

int MySqlAPI::activeProcessesForThisHost()
{

    soci::session sql(*connectionPool);

    unsigned active = 0;
    try
        {
            soci::statement stmt1 = (
                                        sql.prepare << "select count(*) from t_file where TRANSFERHOST=:host AND file_state  = 'ACTIVE'  ", soci::use(hostname), soci::into(active));
            stmt1.execute(true);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return active;
}

std::vector< boost::tuple<std::string, std::string, int> >  MySqlAPI::getVOBringonlineMax()
{

    soci::session sql(*connectionPool);

    std::vector< boost::tuple<std::string, std::string, int> > ret;

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             "SELECT vo_name, host, concurrent_ops FROM t_stage_req"
                                         );

            for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    soci::row const& row = *i;

                    boost::tuple<std::string, std::string, int> item (
                        row.get<std::string>("vo_name",""),
                        row.get<std::string>("host",""),
                        row.get<int>("concurrent_ops",0)
                    );

                    ret.push_back(item);
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return ret;

}


std::vector<message_bringonline> MySqlAPI::getBringOnlineFiles(std::string voName, std::string hostName, int maxValue)
{

    soci::session sql(*connectionPool);

    std::vector<message_bringonline> ret;
    std::string hostV;
    unsigned int currentStagingFilesNoConfig = 0;


    try
        {

            if (voName.empty())
                {

                    soci::rowset<soci::row> rs = (
                                                     sql.prepare <<
                                                     " SELECT distinct(f.source_se) "
                                                     " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                                     " WHERE "
                                                     "	(j.BRING_ONLINE > 0 OR j.COPY_PIN_LIFETIME > 0) "
                                                     "	AND f.file_state = 'STAGING' "
                                                     "	AND f.staging_start IS NULL and j.job_finished is null "
                                                     "  AND (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd)",
                                                     soci::use(hashSegment.start), soci::use(hashSegment.end)
                                                 );

                    soci::statement stmt1 = (sql.prepare << " SELECT COUNT(*) "
                                             " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                             " WHERE "
                                             "       (j.BRING_ONLINE > 0 OR j.COPY_PIN_LIFETIME > 0) "
                                             "	AND f.file_state = 'STAGING' "
                                             "	AND f.staging_start IS NOT NULL "
                                             "	AND f.source_se = :hostV and j.job_finished is null",
                                             soci::use(hostV),
                                             soci::into(currentStagingFilesNoConfig));

                    for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                        {
                            soci::row const& row = *i;

                            hostV = row.get<std::string>("source_se");

                            currentStagingFilesNoConfig = 0;

                            stmt1.execute(true);

                            int maxNoConfig = currentStagingFilesNoConfig > 0 ? maxValue - currentStagingFilesNoConfig : maxValue;
                            if(maxNoConfig > 500 || maxNoConfig < 0)
                                maxNoConfig = 500;

                            soci::rowset<soci::row> rs2 = (
                                                              sql.prepare <<
                                                              " SELECT f.source_surl, f.job_id, f.file_id, j.copy_pin_lifetime, j.bring_online "
                                                              " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                                              " WHERE  "
                                                              " (j.BRING_ONLINE > 0 OR j.COPY_PIN_LIFETIME > 0) "
                                                              "	AND f.staging_start IS NULL "
                                                              "	AND f.file_state = 'STAGING' "
                                                              "	AND f.source_se = :source_se  "
                                                              "	AND j.job_finished is null "
                                                              " LIMIT :limit",
                                                              soci::use(hostV),
                                                              soci::use(maxNoConfig)

                                                          );

                            for (soci::rowset<soci::row>::const_iterator i2 = rs2.begin(); i2 != rs2.end(); ++i2)
                                {
                                    soci::row const& row2 = *i2;

                                    struct message_bringonline msg;
                                    msg.url = row2.get<std::string>("source_surl");
                                    msg.job_id = row2.get<std::string>("job_id");
                                    msg.file_id = row2.get<int>("file_id");
                                    msg.pinlifetime = row2.get<int>("copy_pin_lifetime",0);
                                    msg.bringonlineTimeout = row2.get<int>("bring_online",0);

                                    ret.push_back(msg);
                                    bringOnlineReportStatusInternal(sql, "STARTED", "", msg);
                                }
                        }
                }
            else
                {

                    unsigned currentStagingFilesConfig = 0;

                    sql <<
                        " SELECT COUNT(*) FROM t_job j INNER JOIN t_file f ON (j.job_id = f.job_id) "
                        " WHERE  "
                        " 	(j.BRING_ONLINE > 0 OR j.COPY_PIN_LIFETIME > 0) "
                        "	AND f.file_state = 'STAGING' "
                        "	AND f.STAGING_START IS NOT NULL "
                        " 	AND j.vo_name = :vo_name "
                        "	AND f.source_se = :source_se   "
                        "       AND (f.hashed_id >= :hStart AND f.hashed_id <= :hEnd)",
                        soci::use(voName),
                        soci::use(hostName),
                        soci::use(hashSegment.start), soci::use(hashSegment.end),
                        soci::into(currentStagingFilesConfig)
                        ;

                    int maxConfig = currentStagingFilesConfig > 0 ? maxValue - currentStagingFilesConfig : maxValue;
                    if(maxConfig > 500 || maxConfig < 0)
                        maxConfig = 500;

                    soci::rowset<soci::row> rs = (
                                                     sql.prepare <<
                                                     " SELECT f.source_surl, f.job_id, f.file_id, j.copy_pin_lifetime, j.bring_online "
                                                     " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                                     " WHERE  "
                                                     "	(j.BRING_ONLINE > 0 OR j.COPY_PIN_LIFETIME > 0) "
                                                     " 	AND f.staging_START IS NULL "
                                                     "	AND f.file_state = 'STAGING' "
                                                     "	AND f.source_se = :source_se "
                                                     "	AND j.vo_name = :vo_name   "
                                                     "	AND j.job_finished is null"
                                                     " LIMIT :limit",
                                                     soci::use(hostName),
                                                     soci::use(voName),
                                                     soci::use(maxConfig)
                                                 );

                    for (soci::rowset<soci::row>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                        {
                            soci::row const& row = *i;

                            struct message_bringonline msg;
                            msg.url = row.get<std::string>("source_surl");
                            msg.job_id = row.get<std::string>("job_id");
                            msg.file_id = row.get<int>("file_id");
                            msg.pinlifetime = row.get<int>("copy_pin_lifetime",0);
                            msg.bringonlineTimeout = row.get<int>("bring_online",0);

                            ret.push_back(msg);
                            bringOnlineReportStatusInternal(sql, "STARTED", "", msg);
                        }
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return ret;
}

void MySqlAPI::bringOnlineReportStatusInternal(soci::session& sql,
        const std::string & state, const std::string & message,
        const struct message_bringonline& msg)
{
    std::vector<struct message_state> files;

    try
        {
            if (state == "STARTED")
                {
                    sql.begin();
                    sql <<
                        " UPDATE t_file "
                        " SET staging_start = UTC_TIMESTAMP(), transferhost=:thost, file_state='STARTED' "
                        " WHERE job_id = :jobId "
                        "	AND file_id= :fileId "
                        "	AND file_state='STAGING'",
                        soci::use(hostname),
                        soci::use(msg.job_id),
                        soci::use(msg.file_id)
                        ;
                    sql.commit();
                }
            else
                {
                    std::string source_surl;
                    std::string dest_surl;
                    std::string dbState;
                    std::string dbReason;
                    int stage_in_only = 0;

                    sql << "select count(*) from t_file where job_id=:job_id and file_id=:file_id and source_surl=dest_surl",
                        soci::use(msg.job_id),
                        soci::use(msg.file_id),
                        soci::into(stage_in_only);

                    if(stage_in_only == 0)  //stage-in and transfer
                        {
                            dbState = state == "FINISHED" ? "SUBMITTED" : state;
                            dbReason = state == "FINISHED" ? std::string() : message;
                        }
                    else //stage-in only
                        {
                            dbState = state == "FINISHED" ? "FINISHED" : state;
                            dbReason = state == "FINISHED" ? std::string() : message;
                        }


                    sql.begin();
                    sql <<
                        " UPDATE t_file "
                        " SET staging_finished = UTC_TIMESTAMP(), job_finished=UTC_TIMESTAMP(), finish_time=UTC_TIMESTAMP(), reason = :reason, file_state = :fileState "
                        " WHERE job_id = :jobId "
                        "	AND file_id = :fileId "
                        "	AND file_state in ('STAGING','STARTED')",
                        soci::use(dbReason),
                        soci::use(dbState),
                        soci::use(msg.job_id),
                        soci::use(msg.file_id)
                        ;
                    sql.commit();

                    //check if session reuse has been issued
                    soci::indicator isNull = soci::i_ok;
                    std::string reuse("");
                    sql << " select reuse_job from t_job where job_id=:jobId", soci::use(msg.job_id), soci::into(reuse, isNull);
                    if (isNull != soci::i_null && reuse == "Y")
                        {
                            int countTr = 0;
                            sql << " select count(*) from t_file where job_id=:jobId and file_state in ('STAGING','STARTED') ", soci::use(msg.job_id), soci::into(countTr);
                            if(countTr == 0)
                                {
                                    if(stage_in_only == 0)
                                        {
                                            updateJobTransferStatusInternal(sql, msg.job_id, "SUBMITTED",0);
                                        }
                                    else
                                        {
                                            updateJobTransferStatusInternal(sql, msg.job_id, dbState,0);
                                        }
                                }
                        }
                    else
                        {
                            updateJobTransferStatusInternal(sql, msg.job_id, dbState,0);
                        }
                }

            //send state message
            files = getStateOfTransferInternal(sql, msg.job_id, msg.file_id);
            if(!files.empty())
                {
                    std::vector<struct message_state>::iterator it;
                    for (it = files.begin(); it != files.end(); ++it)
                        {
                            struct message_state tmp = (*it);
                            constructJSONMsg(&tmp);
                        }
                }
            files.clear();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

}

void MySqlAPI::bringOnlineReportStatus(const std::string & state, const std::string & message, const struct message_bringonline& msg)
{
    soci::session sql(*connectionPool);
    try
        {
            bringOnlineReportStatusInternal(sql, state, message, msg);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::addToken(const std::string & job_id, int file_id, const std::string & token)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql <<
                " UPDATE t_file "
                " SET bringonline_token = :token "
                " WHERE job_id = :jobId "
                "	AND file_id = :fileId "
                "	AND file_state = 'STARTED' ",
                soci::use(token),
                soci::use(job_id),
                soci::use(file_id);

            sql.commit();

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::getCredentials(std::string & vo_name, const std::string & job_id, int, std::string & dn, std::string & dlg_id)
{

    soci::session sql(*connectionPool);

    try
        {
            sql <<
                " SELECT vo_name, user_dn, cred_id FROM t_job WHERE job_id = :jobId ",
                soci::use(job_id),
                soci::into(vo_name),
                soci::into(dn),
                soci::into(dlg_id)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::setMaxStageOp(const std::string& se, const std::string& vo, int val)
{
    soci::session sql(*connectionPool);

    try
        {
            int exist = 0;

            sql <<
                " SELECT COUNT(*) "
                " FROM t_stage_req "
                " WHERE vo_name = :vo AND host = :se ",
                soci::use(vo),
                soci::use(se),
                soci::into(exist)
                ;

            sql.begin();

            // if the record already exist ...
            if (exist)
                {
                    // update
                    sql <<
                        " UPDATE t_stage_req "
                        " SET concurrent_ops = :value "
                        " WHERE vo_name = :vo AND host = :se ",
                        soci::use(val),
                        soci::use(vo),
                        soci::use(se)
                        ;
                }
            else
                {
                    // otherwise insert
                    sql <<
                        " INSERT "
                        " INTO t_stage_req (host, vo_name, concurrent_ops) "
                        " VALUES (:se, :vo, :value)",
                        soci::use(se),
                        soci::use(vo),
                        soci::use(val)
                        ;
                }

            sql.commit();

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::updateProtocol(const std::string& /*jobId*/, int fileId, int nostreams, int timeout, int buffersize, double filesize)
{

    std::stringstream internalParams;
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            internalParams << "nostreams:" << nostreams << ",timeout:" << timeout << ",buffersize:" << buffersize;

            sql <<
                " UPDATE t_file set INTERNAL_FILE_PARAMS=:1, FILESIZE=:2 where file_id=:fileId ",
                soci::use(internalParams.str()),
                soci::use(filesize),
                soci::use(fileId);

            sql.commit();

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

double MySqlAPI::getSuccessRate(std::string source, std::string destination)
{
    soci::session sql(*connectionPool);

    double ratioSuccessFailure = 0.0;
    double lastSuccessRate = 0.0;
    soci::indicator isNull = soci::i_ok;

    try
        {
            double nFailedLastHour=0.0, nFinishedLastHour=0.0;

            soci::rowset<std::string> rs = (
                                               sql.prepare <<
                                               "SELECT file_state FROM t_file "
                                               "WHERE "
                                               "      t_file.source_se = :source AND t_file.dest_se = :dst AND "
                                               "      (t_file.job_finished > (UTC_TIMESTAMP() - interval '1' minute)) AND "
                                               "      file_state IN ('FAILED','FINISHED') ",
                                               soci::use(source), soci::use(destination)
                                           )
                                           ;

            soci::statement stmt4 = (
                                        sql.prepare << 	" SELECT filesize from t_optimizer_evolution WHERE source_se=:source and dest_se=:dest "
                                        " and datetime >= (UTC_TIMESTAMP() - INTERVAL '5' minute) "
                                        " order by datetime DESC LIMIT 1 ",
                                        soci::use(source),
                                        soci::use(destination),
                                        soci::into(lastSuccessRate, isNull));

            bool recordsFound = false;
            for (soci::rowset<std::string>::const_iterator i = rs.begin();
                    i != rs.end(); ++i)
                {
                    if      (i->compare("FAILED") == 0)   nFailedLastHour+=1.0;
                    else if (i->compare("FINISHED") == 0) nFinishedLastHour+=1.0;

                    recordsFound = true;
                }

            if(recordsFound)
                {
                    if(nFinishedLastHour > 0.0)
                        {
                            ratioSuccessFailure = ceil(nFinishedLastHour/(nFinishedLastHour + nFailedLastHour) * (100.0/1.0));
                        }
                }
            else      //get last reported success rate
                {
                    stmt4.execute(true);
                    if (isNull == soci::i_ok)
                        {
                            ratioSuccessFailure = lastSuccessRate;
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return ceil(ratioSuccessFailure);
}


double MySqlAPI::getAvgThroughput(std::string source_hostname, std::string destin_hostname)
{
    soci::session sql(*connectionPool);

    double throughput= 0.0;
    double filesize = 0.0;
    double totalSize = 0.0;

    try
        {
            // Weighted average for the 5 newest transfers
            soci::rowset<soci::row>  rsSizeAndThroughput = (sql.prepare <<
                    " SELECT filesize, throughput "
                    " FROM t_file "
                    " WHERE source_se = :source AND dest_se = :dest AND "
                    "       file_state = 'ACTIVE' AND throughput > 0 AND "
                    "       filesize > 0 ",
                    soci::use(source_hostname),soci::use(destin_hostname));

            for (soci::rowset<soci::row>::const_iterator j = rsSizeAndThroughput.begin();
                    j != rsSizeAndThroughput.end(); ++j)
                {
                    filesize    = j->get<double>("filesize", 0.0);
                    throughput += (j->get<double>("throughput", 0.0) * filesize);
                    totalSize  += filesize;
                }
            if (totalSize > 0)
                throughput /= totalSize;

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return throughput;
}

void MySqlAPI::cancelFilesInTheQueue(const std::string& se, const std::string& vo, std::set<std::string>& jobs)
{
    soci::session sql(*connectionPool);
    int fileId = 0;
    std::string jobId;
    int fileIndex = 0;

    try
        {
            soci::rowset<soci::row> rs = vo.empty() ?
                                         (
                                             sql.prepare <<
                                             " SELECT file_id, job_id, file_index "
                                             " FROM t_file "
                                             " WHERE (source_se = :se OR dest_se = :se) "
                                             "	AND file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED')",
                                             soci::use(se),
                                             soci::use(se)
                                         )
                                         :
                                         (
                                             sql.prepare <<
                                             " SELECT f.file_id, f.job_id, file_index "
                                             " FROM t_file f "
                                             " WHERE  (f.source_se = :se OR f.dest_se = :se) "
                                             "	AND f.vo_name = :vo "
                                             " 	AND f.file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED') ",
                                             soci::use(se),
                                             soci::use(se),
                                             soci::use(vo)
                                         );

            soci::statement stmt = (sql.prepare << " UPDATE t_file "
                                    " SET file_state = 'CANCELED' "
                                    " WHERE file_id = :fileId",
                                    soci::use(fileId));

            soci::statement stmt1 = (sql.prepare << " UPDATE t_file "
                                     " SET file_state = 'SUBMITTED' "
                                     " WHERE file_state = 'NOT_USED' "
                                     "	AND job_id = :jobId "
                                     "	AND file_index = :fileIndex "
                                     "	AND NOT EXISTS ( "
                                     "		SELECT NULL "
                                     "		FROM ( "
                                     "			SELECT NULL "
                                     "			FROM t_file "
                                     "			WHERE job_id = :jobId "
                                     "				AND file_index = :fileIndex "
                                     "				AND file_state <> 'NOT_USED' "
                                     "				AND file_state <> 'FAILED' "
                                     "				AND file_state <> 'CANCELED' "
                                     "		) AS tmp "
                                     "	) ",
                                     soci::use(jobId),
                                     soci::use(fileIndex),
                                     soci::use(jobId),
                                     soci::use(fileIndex));

            sql.begin();

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); ++it)
                {

                    jobId = it->get<std::string>("job_id");
                    fileId = it->get<int>("file_id");

                    jobs.insert(jobId);

                    stmt.execute(true);

                    fileIndex = it->get<int>("file_index");

                    stmt1.execute(true);
                }

            sql.commit();

            std::set<std::string>::iterator job_it;
            for (job_it = jobs.begin(); job_it != jobs.end(); ++job_it)
                {
                    updateJobTransferStatusInternal(sql, *job_it, std::string(),0);
                }

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

void MySqlAPI::cancelJobsInTheQueue(const std::string& dn, std::vector<std::string>& jobs)
{
    soci::session sql(*connectionPool);

    try
        {

            // bare in mind that in this case we do not care about NOT_USED because we are operating at the level of a job

            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT job_id "
                                             " FROM t_job "
                                             " WHERE user_dn = :dn "
                                             "	AND job_state IN ('ACTIVE', 'READY', 'SUBMITTED')",
                                             soci::use(dn)
                                         );

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); ++it)
                {

                    jobs.push_back(it->get<std::string>("job_id"));
                }

            cancelJob(jobs);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



void MySqlAPI::transferLogFileVector(std::map<int, struct message_log>& messagesLog)
{
    soci::session sql(*connectionPool);

    std::string filePath;
    //soci doesn't access bool
    unsigned int debugFile = 0;
    int fileId = 0;

    try
        {
            soci::statement stmt = (sql.prepare << " update t_file set t_log_file=:filePath, t_log_file_debug=:debugFile where file_id=:fileId and file_state<>'SUBMITTED' ",
                                    soci::use(filePath),
                                    soci::use(debugFile),
                                    soci::use(fileId));

            sql.begin();

            std::map<int, struct message_log>::iterator iterLog = messagesLog.begin();
            while (iterLog != messagesLog.end())
                {
                    filePath = ((*iterLog).second).filePath;
                    fileId = ((*iterLog).second).file_id;
                    debugFile = ((*iterLog).second).debugFile;
                    stmt.execute(true);

                    if (stmt.get_affected_rows() > 0)
                        {
                            // erase
                            messagesLog.erase(iterLog++);
                        }
                    else
                        {
                            ++iterLog;
                        }
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

std::vector<struct message_state> MySqlAPI::getStateOfTransferInternal(soci::session& sql, const std::string& jobId, int fileId)
{
    message_state ret;
    soci::indicator ind = soci::i_ok;
    std::vector<struct message_state> temp;
    int retry = 0;

    try
        {
            sql << " select retry from t_server_config LIMIT 1", soci::into(retry, ind);
            if (ind == soci::i_null)
                {
                    retry = 0;
                }

            soci::rowset<soci::row> rs = (fileId ==-1) ? (
                                             sql.prepare <<
                                             " SELECT "
                                             "	j.job_id, j.job_state, j.vo_name, "
                                             "	j.job_metadata, j.retry AS retry_max, f.file_id, "
                                             "	f.file_state, f.retry AS retry_counter, f.file_metadata, "
                                             "	f.source_se, f.dest_se "
                                             " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                             " WHERE "
                                             " 	j.job_id = :jobId ",
                                             soci::use(jobId)
                                         )
                                         :
                                         (
                                             sql.prepare <<
                                             " SELECT "
                                             "	j.job_id, j.job_state, j.vo_name, "
                                             "	j.job_metadata, j.retry AS retry_max, f.file_id, "
                                             "	f.file_state, f.retry AS retry_counter, f.file_metadata, "
                                             "	f.source_se, f.dest_se "
                                             " FROM t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                             " WHERE "
                                             " 	j.job_id = :jobId "
                                             "  AND f.file_id = :fileId ",
                                             soci::use(jobId),
                                             soci::use(fileId)
                                         );


            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); ++it)
                {
                    ret.job_id = it->get<std::string>("job_id");
                    ret.job_state = it->get<std::string>("job_state");
                    ret.vo_name = it->get<std::string>("vo_name");
                    ret.job_metadata = it->get<std::string>("job_metadata","");
                    ret.retry_max = it->get<int>("retry_max",0);
                    ret.file_id = it->get<int>("file_id");
                    ret.file_state = it->get<std::string>("file_state");
                    ret.retry_counter = it->get<int>("retry_counter",0);
                    ret.file_metadata = it->get<std::string>("file_metadata","");
                    ret.source_se = it->get<std::string>("source_se");
                    ret.dest_se = it->get<std::string>("dest_se");
                    ret.timestamp = getStrUTCTimestamp();

                    if(ret.retry_max == 0)
                        {
                            ret.retry_max = retry;
                        }

                    temp.push_back(ret);
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return temp;

}

std::vector<struct message_state> MySqlAPI::getStateOfTransfer(const std::string& jobId, int fileId)
{
    soci::session sql(*connectionPool);
    std::vector<struct message_state> temp;

    try
        {
            temp = getStateOfTransferInternal(sql, jobId, fileId);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return temp;
}

void MySqlAPI::getFilesForJob(const std::string& jobId, std::vector<int>& files)
{
    soci::session sql(*connectionPool);

    try
        {

            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT file_id "
                                             " FROM t_file "
                                             " WHERE job_id = :jobId",
                                             soci::use(jobId)
                                         );

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); ++it)
                {
                    files.push_back(
                        it->get<int>("file_id")
                    );
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::getFilesForJobInCancelState(const std::string& jobId, std::vector<int>& files)
{
    soci::session sql(*connectionPool);

    try
        {

            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT file_id "
                                             " FROM t_file "
                                             " WHERE job_id = :jobId "
                                             "	AND file_state = 'CANCELED' ",
                                             soci::use(jobId)
                                         );

            soci::rowset<soci::row>::const_iterator it;
            for (it = rs.begin(); it != rs.end(); ++it)
                {
                    files.push_back(
                        it->get<int>("file_id")
                    );
                }

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::setFilesToWaiting(const std::string& se, const std::string& vo, int timeout)
{
    soci::session sql(*connectionPool);

    try
        {

            sql.begin();

            if (vo.empty())
                {

                    sql <<
                        " UPDATE t_file "
                        " SET wait_timestamp = UTC_TIMESTAMP(), wait_timeout = :timeout "
                        " WHERE (source_se = :src OR dest_se = :dest) "
                        "	AND file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED') "
                        "	AND (wait_timestamp IS NULL OR wait_timeout IS NULL) ",
                        soci::use(timeout),
                        soci::use(se),
                        soci::use(se)
                        ;

                }
            else
                {

                    sql <<
                        " UPDATE t_file f, t_job j "
                        " SET f.wait_timestamp = UTC_TIMESTAMP(), f.wait_timeout = :timeout "
                        " WHERE (f.source_se = :src OR f.dest_se = :dest) "
                        "	AND j.vo_name = :vo "
                        "	AND f.file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED') "
                        "	AND f.job_id = j.job_id "
                        "	AND (f.wait_timestamp IS NULL OR f.wait_timeout IS NULL) ",
                        soci::use(timeout),
                        soci::use(se),
                        soci::use(se),
                        soci::use(vo)
                        ;
                }

            sql.commit();

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::setFilesToWaiting(const std::string& dn, int timeout)
{
    soci::session sql(*connectionPool);

    try
        {

            sql.begin();

            sql <<
                " UPDATE t_file f, t_job j "
                " SET f.wait_timestamp = UTC_TIMESTAMP(), f.wait_timeout = :timeout "
                " WHERE j.user_dn = :dn "
                "	AND f.file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED') "
                "	AND f.job_id = j.job_id "
                "	AND (f.wait_timestamp IS NULL OR f.wait_timeout IS NULL) ",
                soci::use(timeout),
                soci::use(dn)
                ;

            sql.commit();

        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::cancelWaitingFiles(std::set<std::string>& jobs)
{

    soci::session sql(*connectionPool);

    try
        {
            soci::rowset<soci::row> rs = (
                                             sql.prepare <<
                                             " SELECT file_id, job_id "
                                             " FROM t_file "
                                             " WHERE wait_timeout <> 0 "
                                             "	AND TIMESTAMPDIFF(SECOND, wait_timestamp, UTC_TIMESTAMP()) > wait_timeout "
                                             "	AND file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED')"
                                             "  AND (hashed_id >= :hStart AND hashed_id <= :hEnd) ",
                                             soci::use(hashSegment.start), soci::use(hashSegment.end)
                                         );

            soci::rowset<soci::row>::iterator it;

            sql.begin();
            for (it = rs.begin(); it != rs.end(); ++it)
                {

                    jobs.insert(it->get<std::string>("job_id"));

                    sql <<
                        " UPDATE t_file "
                        " SET file_state = 'CANCELED', finish_time = UTC_TIMESTAMP() "
                        " WHERE file_id = :fileId AND file_state IN ('ACTIVE', 'READY', 'SUBMITTED', 'NOT_USED')",
                        soci::use(it->get<int>("file_id"))
                        ;
                }

            sql.commit();

            std::set<std::string>::iterator job_it;
            for (job_it = jobs.begin(); job_it != jobs.end(); ++job_it)
                {
                    updateJobTransferStatusInternal(sql, *job_it, std::string(),0);
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::revertNotUsedFiles()
{

    soci::session sql(*connectionPool);
    std::string notUsed;

    try
        {
            soci::rowset<std::string> rs = (
                                               sql.prepare <<
                                               "select distinct f.job_id from t_file f INNER JOIN t_job j ON (f.job_id = j.job_id) "
                                               " WHERE file_state = 'NOT_USED' and j.job_finished is NULL "
                                               "  AND (hashed_id >= :hStart AND hashed_id <= :hEnd) ",
                                               soci::use(hashSegment.start), soci::use(hashSegment.end)
                                           );


            soci::statement st((sql.prepare << " UPDATE t_file f1 "
                                " SET f1.file_state = 'SUBMITTED' "
                                " WHERE f1.file_state = 'NOT_USED' "
                                "	AND NOT EXISTS ( "
                                "		SELECT NULL "
                                "		FROM ( "
                                "			SELECT job_id, file_index, file_state "
                                "			FROM t_file f2 "
                                "			WHERE f2.job_id = :notUsed AND EXISTS ( "
                                "					SELECT NULL "
                                "					FROM t_file f3 "
                                "					WHERE f2.job_id = f3.job_id "
                                "						AND f3.file_index = f2.file_index "
                                "						AND f3.file_state = 'NOT_USED' "
                                "				) "
                                "				AND f2.file_state <> 'NOT_USED' "
                                "				AND f2.file_state <> 'CANCELED' "
                                "				AND f2.file_state <> 'FAILED' "
                                "		) AS t_file_tmp "
                                "		WHERE t_file_tmp.job_id = f1.job_id "
                                "			AND t_file_tmp.file_index = f1.file_index "
                                "	) ", soci::use(notUsed)
                               ));

            sql.begin();
            for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    notUsed = *i;
                    st.execute(true);
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

bool MySqlAPI::isShareOnly(std::string se)
{

    soci::session sql(*connectionPool);

    bool ret = true;
    try
        {
            sql <<
                " select symbolicName from t_link_config "
                " where source = :source and destination = '*' and auto_tuning = 'all'",
                soci::use(se)
                ;

            ret = sql.got_data();

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

std::vector<std::string> MySqlAPI::getAllShareOnlyCfgs()
{

    soci::session sql(*connectionPool);

    std::vector<std::string> ret;

    try
        {
            soci::rowset<std::string> rs = (
                                               sql.prepare <<
                                               " select source "
                                               " from t_link_config "
                                               " where destination = '*' and auto_tuning = 'all' "
                                           );

            for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    ret.push_back(*i);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return ret;
}

void MySqlAPI::checkSanityState()
{
    //TERMINAL STATES:  "FINISHED" FAILED" "CANCELED"
    soci::session sql(*connectionPool);

    unsigned int numberOfFiles = 0;
    unsigned int terminalState = 0;
    unsigned int allFinished = 0;
    unsigned int allFailed = 0;
    unsigned int allCanceled = 0;
    unsigned int numberOfFilesRevert = 0;
    std::string canceledMessage = "Transfer canceled by the user";
    std::string failed = "One or more files failed. Please have a look at the details for more information";
    std::string job_id;

    try
        {
            if(hashSegment.start == 0)
                {
                    soci::rowset<std::string> rs = (
                                                       sql.prepare <<
                                                       " select job_id from t_job  where job_finished is null "
                                                   );

                    soci::statement stmt1 = (sql.prepare << "SELECT COUNT(DISTINCT file_index) FROM t_file where job_id=:jobId ", soci::use(job_id), soci::into(numberOfFiles));

                    soci::statement stmt2 = (sql.prepare << "UPDATE t_job SET "
                                             "    job_state = 'CANCELED', job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), "
                                             "    reason = :canceledMessage "
                                             "    WHERE job_id = :jobId ", soci::use(canceledMessage), soci::use(job_id));

                    soci::statement stmt3 = (sql.prepare << "UPDATE t_job SET "
                                             "    job_state = 'FINISHED', job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP() "
                                             "    WHERE job_id = :jobId", soci::use(job_id));

                    soci::statement stmt4 = (sql.prepare << "UPDATE t_job SET "
                                             "    job_state = 'FAILED', job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), "
                                             "    reason = :failed "
                                             "    WHERE job_id = :jobId", soci::use(failed), soci::use(job_id));


                    soci::statement stmt5 = (sql.prepare << "UPDATE t_job SET "
                                             "    job_state = 'FINISHEDDIRTY', job_finished = UTC_TIMESTAMP(), finish_time = UTC_TIMESTAMP(), "
                                             "    reason = :failed "
                                             "    WHERE job_id = :jobId", soci::use(failed), soci::use(job_id));

                    soci::statement stmt6 = (sql.prepare << "SELECT COUNT(*) FROM t_file where job_id=:jobId AND file_state in ('ACTIVE','READY','SUBMITTED','STAGING') ", soci::use(job_id), soci::into(numberOfFilesRevert));

                    soci::statement stmt7 = (sql.prepare << "UPDATE t_job SET "
                                             "    job_state = 'SUBMITTED', job_finished = NULL, finish_time = NULL, "
                                             "    reason = NULL "
                                             "    WHERE job_id = :jobId", soci::use(job_id));

                    soci::statement stmt8 = (sql.prepare << " select count(*)  "
                                             " from t_file "
                                             " where job_id = :jobId "
                                             "	and  file_state = 'FINISHED' ",
                                             soci::use(job_id),
                                             soci::into(allFinished));

                    soci::statement stmt9 = (sql.prepare << " select count(distinct f1.file_index) "
                                             " from t_file f1 "
                                             " where f1.job_id = :jobId "
                                             "	and f1.file_state = 'CANCELED' "
                                             "	and NOT EXISTS ( "
                                             "		select null "
                                             "		from t_file f2 "
                                             "		where f2.job_id = :jobId "
                                             "			and f1.file_index = f2.file_index "
                                             "			and f2.file_state <> 'CANCELED' "
                                             " 	) ",
                                             soci::use(job_id),
                                             soci::use(job_id),
                                             soci::into(allCanceled));

                    soci::statement stmt10 = (sql.prepare << " select count(distinct f1.file_index) "
                                              " from t_file f1 "
                                              " where f1.job_id = :jobId "
                                              "	and f1.file_state = 'FAILED' "
                                              "	and NOT EXISTS ( "
                                              "		select null "
                                              "		from t_file f2 "
                                              "		where f2.job_id = :jobId "
                                              "			and f1.file_index = f2.file_index "
                                              "			and f2.file_state NOT IN ('CANCELED', 'FAILED') "
                                              " 	) ",
                                              soci::use(job_id),
                                              soci::use(job_id),
                                              soci::into(allFailed));

                    sql.begin();
                    for (soci::rowset<std::string>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                        {
                            job_id = (*i);
                            numberOfFiles = 0;
                            allFinished = 0;
                            allCanceled = 0;
                            allFailed = 0;
                            terminalState = 0;

                            stmt1.execute(true);

                            if(numberOfFiles > 0)
                                {
                                    stmt8.execute(true);
                                    stmt9.execute(true);
                                    stmt10.execute(true);

                                    terminalState = allFinished + allCanceled + allFailed;

                                    if(numberOfFiles == terminalState)  /* all files terminal state but job in ('ACTIVE','READY','SUBMITTED','STAGING') */
                                        {
                                            if(allCanceled > 0)
                                                {
                                                    stmt2.execute(true);
                                                }
                                            else   //non canceled, check other states: "FINISHED" and FAILED"
                                                {
                                                    if(numberOfFiles == allFinished)  /*all files finished*/
                                                        {
                                                            stmt3.execute(true);
                                                        }
                                                    else
                                                        {
                                                            if(numberOfFiles == allFailed)  /*all files failed*/
                                                                {
                                                                    stmt4.execute(true);
                                                                }
                                                            else   // otherwise it is FINISHEDDIRTY
                                                                {
                                                                    stmt5.execute(true);
                                                                }
                                                        }
                                                }
                                        }
                                }
                        }
                    sql.commit();


                    //now check reverse sanity checks, JOB can't be FINISH,  FINISHEDDIRTY, FAILED is at least one tr is in SUBMITTED, READY, ACTIVE
                    //special case for canceled
                    soci::rowset<std::string> rs2 = (
                                                        sql.prepare <<
                                                        " select job_id from t_job where job_finished is not null  "
                                                    );

                    sql.begin();
                    for (soci::rowset<std::string>::const_iterator i2 = rs2.begin(); i2 != rs2.end(); ++i2)
                        {
                            job_id = (*i2);
                            numberOfFilesRevert = 0;

                            stmt6.execute(true);

                            if(numberOfFilesRevert > 0)
                                {
                                    stmt7.execute(true);
                                }
                        }
                    sql.commit();

                    //now check if a host has been offline for more than 30 min and set its transfers to failed
                    soci::rowset<std::string> rsCheckHosts = (
                                sql.prepare <<
                                " SELECT hostname "
                                " FROM t_hosts "
                                " WHERE beat < DATE_SUB(UTC_TIMESTAMP(), interval 59 minute) and service_name = 'fts_server' "
                            );

                    std::vector<struct message_state> files;

                    for (soci::rowset<std::string>::const_iterator irsCheckHosts = rsCheckHosts.begin(); irsCheckHosts != rsCheckHosts.end(); ++irsCheckHosts)
                        {
                            std::string deadHost = (*irsCheckHosts);

                            //now check and collect if there are any active/ready in these hosts
                            soci::rowset<soci::row> rsCheckHostsActive = (
                                        sql.prepare <<
                                        " SELECT file_id, job_id from t_file where file_state in ('READY','ACTIVE') and transferHost = :transferHost ", soci::use(deadHost)
                                    );
                            for (soci::rowset<soci::row>::const_iterator iCheckHostsActive = rsCheckHostsActive.begin(); iCheckHostsActive != rsCheckHostsActive.end(); ++iCheckHostsActive)
                                {
                                    int file_id = iCheckHostsActive->get<int>("file_id");
                                    std::string job_id = iCheckHostsActive->get<std::string>("job_id");
                                    std::string errorMessage = "Transfer has been forced-canceled because host " + deadHost + " is offline and transfers still assigned to it";

                                    updateFileTransferStatusInternal(sql, 0.0, job_id, file_id, "CANCELED", errorMessage, 0, 0, 0, false);
                                    updateJobTransferStatusInternal(sql, job_id, "CANCELED",0);

                                    //send state monitoring message for the state transition
                                    files = getStateOfTransferInternal(sql, job_id, file_id);
                                    if(!files.empty())
                                        {
                                            std::vector<struct message_state>::iterator it;
                                            for (it = files.begin(); it != files.end(); ++it)
                                                {
                                                    struct message_state tmp = (*it);
                                                    constructJSONMsg(&tmp);
                                                }
                                            files.clear();
                                        }
                                }
                        }
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



void MySqlAPI::getFilesForNewSeCfg(std::string source, std::string destination, std::string vo, std::vector<int>& out)
{

    soci::session sql(*connectionPool);

    try
        {
            soci::rowset<int> rs = (
                                       sql.prepare <<
                                       " select f.file_id "
                                       " from t_file f  "
                                       " where f.source_se like :source "
                                       "	and f.dest_se like :destination "
                                       "	and f.vo_name = :vo "
                                       "	and f.file_state in ('READY', 'ACTIVE') ",
                                       soci::use(source == "*" ? "%" : source),
                                       soci::use(destination == "*" ? "%" : destination),
                                       soci::use(vo)
                                   );

            for (soci::rowset<int>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    out.push_back(*i);
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::getFilesForNewGrCfg(std::string source, std::string destination, std::string vo, std::vector<int>& out)
{
    soci::session sql(*connectionPool);

    std::string select;
    select +=
        " select f.file_id "
        " from t_file f "
        " where "
        "	f.vo_name = :vo "
        "	and f.file_state in ('READY', 'ACTIVE')  ";
    if (source != "*")
        select +=
            "	and f.source_se in ( "
            "		select member "
            "		from t_group_members "
            "		where groupName = :source "
            "	) ";
    if (destination != "*")
        select +=
            "	and f.dest_se in ( "
            "		select member "
            "		from t_group_members "
            "		where groupName = :dest "
            "	) ";

    try
        {
            int id;

            soci::statement stmt(sql);
            stmt.exchange(soci::use(vo, "vo"));
            if (source != "*") stmt.exchange(soci::use(source, "source"));
            if (destination != "*") stmt.exchange(soci::use(destination, "dest"));
            stmt.exchange(soci::into(id));
            stmt.alloc();
            stmt.prepare(select);
            stmt.define_and_bind();

            if (stmt.execute(true))
                {
                    do
                        {
                            out.push_back(id);
                        }
                    while (stmt.fetch());
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::delFileShareConfig(int file_id, std::string source, std::string destination, std::string vo)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            sql <<
                " delete from t_file_share_config  "
                " where file_id = :id "
                "	and source = :src "
                "	and destination = :dest "
                "	and vo = :vo",
                soci::use(file_id),
                soci::use(source),
                soci::use(destination),
                soci::use(vo)
                ;
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::delFileShareConfig(std::string group, std::string se)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql <<
                " delete from t_file_share_config  "
                " where (source = :gr or destination = :gr) "
                "	and file_id IN ( "
                "		select file_id "
                "		from t_file "
                "		where (source_se = :se or dest_se = :se) "
                "			and file_state in ('READY', 'ACTIVE')"
                "	) ",
                soci::use(group),
                soci::use(group),
                soci::use(se),
                soci::use(se)
                ;

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


bool MySqlAPI::hasStandAloneSeCfgAssigned(int file_id, std::string vo)
{
    soci::session sql(*connectionPool);

    int count = 0;

    try
        {
            sql <<
                " select count(*) "
                " from t_file_share_config fc "
                " where fc.file_id = :id "
                "	and fc.vo = :vo "
                "	and ((fc.source <> '(*)' and fc.destination = '*') or (fc.source = '*' and fc.destination <> '(*)')) "
                "	and not exists ( "
                "		select null "
                "		from t_group_members g "
                "		where (g.groupName = fc.source or g.groupName = fc.destination) "
                "	) ",
                soci::use(file_id),
                soci::use(vo),
                soci::into(count)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return count > 0;
}

bool MySqlAPI::hasPairSeCfgAssigned(int file_id, std::string vo)
{
    soci::session sql(*connectionPool);

    int count = 0;

    try
        {
            sql <<
                " select count(*) "
                " from t_file_share_config fc "
                " where fc.file_id = :id "
                "	and fc.vo = :vo "
                "	and (fc.source <> '(*)' and fc.source <> '*' and fc.destination <> '*' and fc.destination <> '(*)') "
                "	and not exists ( "
                "		select null "
                "		from t_group_members g "
                "		where (g.groupName = fc.source or g.groupName = fc.destination) "
                "	) ",
                soci::use(file_id),
                soci::use(vo),
                soci::into(count)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return count > 0;
}



bool MySqlAPI::hasPairGrCfgAssigned(int file_id, std::string vo)
{
    soci::session sql(*connectionPool);

    int count = 0;

    try
        {
            sql <<
                " select count(*) "
                " from t_file_share_config fc "
                " where fc.file_id = :id "
                "	and fc.vo = :vo "
                "	and (fc.source <> '(*)' and fc.source <> '*' and fc.destination <> '*' and fc.destination <> '(*)') "
                "	and exists ( "
                "		select null "
                "		from t_group_members g "
                "		where (g.groupName = fc.source or g.groupName = fc.destination) "
                "	) ",
                soci::use(file_id),
                soci::use(vo),
                soci::into(count)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

    return count > 0;
}


void MySqlAPI::checkSchemaLoaded()
{
    soci::session sql(*connectionPool);

    int count = 0;

    try
        {
            sql <<
                " select count(*) "
                " from t_debug",
                soci::into(count)
                ;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::storeProfiling(const fts3::ProfilingSubsystem* prof)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            sql << "UPDATE t_profiling_snapshot SET "
                "    cnt = 0, exceptions = 0, total = 0, average = 0";

            std::map<std::string, fts3::Profile> profiles = prof->getProfiles();
            std::map<std::string, fts3::Profile>::const_iterator i;
            for (i = profiles.begin(); i != profiles.end(); ++i)
                {
                    sql << "INSERT INTO t_profiling_snapshot (scope, cnt, exceptions, total, average) "
                        "VALUES (:scope, :cnt, :exceptions, :total, :avg) "
                        "ON DUPLICATE KEY UPDATE "
                        "    cnt = :cnt, exceptions = :exceptions, total = :total, average = :avg",
                        soci::use(i->second.nCalled, "cnt"), soci::use(i->second.nExceptions, "exceptions"),
                        soci::use(i->second.totalTime, "total"), soci::use(i->second.getAverage(), "avg"),
                        soci::use(i->first, "scope");
                }


            soci::statement update(sql);
            update.exchange(soci::use(prof->getInterval()));
            update.alloc();

            update.prepare("UPDATE t_profiling_info SET "
                           "    updated = UTC_TIMESTAMP(), period = :period");

            update.define_and_bind();
            update.execute(true);

            if (update.get_affected_rows() == 0)
                {
                    sql << "INSERT INTO t_profiling_info (updated, period) "
                        "VALUES (UTC_TIMESTAMP(), :period)",
                        soci::use(prof->getInterval());
                }

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

void MySqlAPI::setOptimizerMode(int mode)
{

    soci::session sql(*connectionPool);
    int _mode = 0;

    try
        {
            sql << "select count(*) from t_optimize_mode", soci::into(_mode);
            if (_mode == 0)
                {
                    sql.begin();

                    sql << "INSERT INTO t_optimize_mode (mode_opt) VALUES (:mode) ON DUPLICATE KEY UPDATE mode_opt=:mode",
                        soci::use(mode),
                        soci::use(mode)
                        ;

                    sql.commit();

                }
            else
                {
                    sql.begin();

                    sql << "update t_optimize_mode set mode_opt = :mode",
                        soci::use(mode)
                        ;

                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

int MySqlAPI::getOptimizerDefaultMode(soci::session& sql)
{
    int modeDefault = 1;
    int mode = 0;
    soci::indicator ind = soci::i_ok;

    try
        {
            sql <<
                " select mode_opt "
                " from t_optimize_mode LIMIT 1",
                soci::into(mode, ind)
                ;

            if (ind == soci::i_ok)
                {
                    if(mode == 0)
                        return mode + 1;
                    else
                        return mode;
                }
            return modeDefault;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught mode exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return modeDefault;
}


int MySqlAPI::getOptimizerMode(soci::session& sql)
{
    int modeDefault = 2;
    int mode = 0;
    soci::indicator ind = soci::i_ok;

    try
        {
            sql <<
                " select mode_opt "
                " from t_optimize_mode LIMIT 1",
                soci::into(mode, ind)
                ;

            if (ind == soci::i_ok)
                {

                    if(mode==1)
                        {
                            return modeDefault;
                        }
                    else if(mode==2)
                        {
                            return (modeDefault *2);
                        }
                    else if(mode==3)
                        {
                            return (modeDefault *3);
                        }
                    else
                        {
                            return modeDefault;
                        }
                }
            return modeDefault;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught mode exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return modeDefault;
}

void MySqlAPI::setRetryTransfer(const std::string & jobId, int fileId, int retry, const std::string& reason)
{
    soci::session sql(*connectionPool);

    //expressed in secs, default delay
    const int default_retry_delay = 120;
    int retry_delay = 0;
    std::string reuse_job;
    soci::indicator ind = soci::i_ok;

    try
        {
            sql <<
                " select RETRY_DELAY, reuse_job  from t_job where job_id=:jobId ",
                soci::use(jobId),
                soci::into(retry_delay),
                soci::into(reuse_job, ind)
                ;

            sql.begin();

            if ( (ind == soci::i_ok) && reuse_job == "Y")
                {

                    sql << "UPDATE t_job SET "
                        "    job_state = 'ACTIVE' "
                        "WHERE job_id = :jobId AND "
                        "      job_state NOT IN ('FINISHEDDIRTY','FAILED','CANCELED','FINISHED') AND "
                        "      reuse_job = 'Y'",
                        soci::use(jobId);
                }


            if (retry_delay > 0)
                {
                    // update
                    time_t now = getUTC(retry_delay);
                    struct tm tTime;
                    gmtime_r(&now, &tTime);

                    sql << "UPDATE t_file SET retry_timestamp=:1, retry = :retry, file_state = 'SUBMITTED', start_time=NULL, transferHost=NULL, t_log_file=NULL,"
                        " t_log_file_debug=NULL, throughput = 0, current_failures = 1 "
                        " WHERE  file_id = :fileId AND  job_id = :jobId AND file_state NOT IN ('FINISHED','SUBMITTED','FAILED','CANCELED')",
                        soci::use(tTime), soci::use(retry), soci::use(fileId), soci::use(jobId);
                }
            else
                {
                    // update
                    time_t now = getUTC(default_retry_delay);
                    struct tm tTime;
                    gmtime_r(&now, &tTime);

                    sql << "UPDATE t_file SET retry_timestamp=:1, retry = :retry, file_state = 'SUBMITTED', start_time=NULL, transferHost=NULL, "
                        " t_log_file=NULL, t_log_file_debug=NULL, throughput = 0,  current_failures = 1 "
                        " WHERE  file_id = :fileId AND  job_id = :jobId AND file_state NOT IN ('FINISHED','SUBMITTED','FAILED','CANCELED')",
                        soci::use(tTime), soci::use(retry), soci::use(fileId), soci::use(jobId);
                }

            // Keep log
            sql << "INSERT IGNORE INTO t_file_retry_errors "
                "    (file_id, attempt, datetime, reason) "
                "VALUES (:fileId, :attempt, UTC_TIMESTAMP(), :reason)",
                soci::use(fileId), soci::use(retry), soci::use(reason);

            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

void MySqlAPI::getTransferRetries(int fileId, std::vector<FileRetry*>& retries)
{
    soci::session sql(*connectionPool);

    try
        {
            soci::rowset<FileRetry> rs = (sql.prepare << "SELECT * FROM t_file_retry_errors WHERE file_id = :fileId",
                                          soci::use(fileId));


            for (soci::rowset<FileRetry>::const_iterator i = rs.begin(); i != rs.end(); ++i)
                {
                    FileRetry const &retry = *i;
                    retries.push_back(new FileRetry(retry));
                }
        }
    catch (std::exception& e)
        {
            std::vector< FileRetry* >::iterator it;
            for (it = retries.begin(); it != retries.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            retries.clear();

            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            std::vector< FileRetry* >::iterator it;
            for (it = retries.begin(); it != retries.end(); ++it)
                {
                    if(*it)
                        delete (*it);
                }
            retries.clear();

            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

bool MySqlAPI::assignSanityRuns(soci::session& sql, struct message_sanity &msg)
{

    long long rows = 0;

    try
        {
            if(msg.checkSanityState)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set checkSanityState=1, t_checkSanityState = UTC_TIMESTAMP() "
                                        "where checkSanityState=0"
                                        " AND (t_checkSanityState < (UTC_TIMESTAMP() - INTERVAL '30' minute)) "));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.checkSanityState = (rows > 0? true: false);
                    sql.commit();
                    return msg.checkSanityState;
                }
            else if(msg.setToFailOldQueuedJobs)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set setToFailOldQueuedJobs=1, t_setToFailOldQueuedJobs = UTC_TIMESTAMP() "
                                        " where setToFailOldQueuedJobs=0"
                                        " AND (t_setToFailOldQueuedJobs < (UTC_TIMESTAMP() - INTERVAL '15' minute)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.setToFailOldQueuedJobs = (rows > 0? true: false);
                    sql.commit();
                    return msg.setToFailOldQueuedJobs;
                }
            else if(msg.forceFailTransfers)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set forceFailTransfers=1, t_forceFailTransfers = UTC_TIMESTAMP() "
                                        " where forceFailTransfers=0"
                                        " AND (t_forceFailTransfers < (UTC_TIMESTAMP() - INTERVAL '15' minute)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.forceFailTransfers = (rows > 0? true: false);
                    sql.commit();
                    return msg.forceFailTransfers;
                }
            else if(msg.revertToSubmitted)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set revertToSubmitted=1, t_revertToSubmitted = UTC_TIMESTAMP() "
                                        " where revertToSubmitted=0"
                                        " AND (t_revertToSubmitted < (UTC_TIMESTAMP() - INTERVAL '15' minute)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.revertToSubmitted = (rows > 0? true: false);
                    sql.commit();
                    return msg.revertToSubmitted;
                }
            else if(msg.cancelWaitingFiles)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set cancelWaitingFiles=1, t_cancelWaitingFiles = UTC_TIMESTAMP() "
                                        "  where cancelWaitingFiles=0"
                                        " AND (t_cancelWaitingFiles < (UTC_TIMESTAMP() - INTERVAL '15' minute)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.cancelWaitingFiles = (rows > 0? true: false);
                    sql.commit();
                    return msg.cancelWaitingFiles;
                }
            else if(msg.revertNotUsedFiles)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set revertNotUsedFiles=1, t_revertNotUsedFiles = UTC_TIMESTAMP() "
                                        " where revertNotUsedFiles=0"
                                        " AND (t_revertNotUsedFiles < (UTC_TIMESTAMP() - INTERVAL '15' minute)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.revertNotUsedFiles = (rows > 0? true: false);
                    sql.commit();
                    return msg.revertNotUsedFiles;
                }
            else if(msg.cleanUpRecords)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set cleanUpRecords=1, t_cleanUpRecords = UTC_TIMESTAMP() "
                                        " where cleanUpRecords=0"
                                        " AND (t_cleanUpRecords < (UTC_TIMESTAMP() - INTERVAL '3' day)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.cleanUpRecords = (rows > 0? true: false);
                    sql.commit();
                    return msg.cleanUpRecords;
                }
            else if(msg.msgCron)
                {
                    sql.begin();
                    soci::statement st((sql.prepare << "update t_server_sanity set msgcron=1, t_msgcron = UTC_TIMESTAMP() "
                                        " where msgcron=0"
                                        " AND (t_msgcron < (UTC_TIMESTAMP() - INTERVAL '1' day)) "
                                       ));
                    st.execute(true);
                    rows = st.get_affected_rows();
                    msg.msgCron = (rows > 0? true: false);
                    sql.commit();
                    return msg.msgCron;
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return false;
}


void MySqlAPI::resetSanityRuns(soci::session& sql, struct message_sanity &msg)
{
    try
        {
            sql.begin();
            if(msg.checkSanityState)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set checkSanityState=0 where (checkSanityState=1 "
                                        " OR (t_checkSanityState < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.setToFailOldQueuedJobs)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set setToFailOldQueuedJobs=0 where (setToFailOldQueuedJobs=1 "
                                        " OR (t_setToFailOldQueuedJobs < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.forceFailTransfers)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set forceFailTransfers=0 where (forceFailTransfers=1 "
                                        " OR (t_forceFailTransfers < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.revertToSubmitted)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set revertToSubmitted=0 where (revertToSubmitted=1  "
                                        " OR (t_revertToSubmitted < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.cancelWaitingFiles)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set cancelWaitingFiles=0 where (cancelWaitingFiles=1  "
                                        " OR (t_cancelWaitingFiles < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.revertNotUsedFiles)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set revertNotUsedFiles=0 where (revertNotUsedFiles=1  "
                                        " OR (t_revertNotUsedFiles < (UTC_TIMESTAMP() - INTERVAL '45' minute)))  "));
                    st.execute(true);
                }
            else if(msg.cleanUpRecords)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set cleanUpRecords=0 where (cleanUpRecords=1  "
                                        " OR (t_cleanUpRecords < (UTC_TIMESTAMP() - INTERVAL '4' day)))  "));
                    st.execute(true);
                }
            else if(msg.msgCron)
                {
                    soci::statement st((sql.prepare << "update t_server_sanity set msgcron=0 where msgcron=1"));
                    st.execute(true);
                }
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}



void MySqlAPI::updateHeartBeat(unsigned* index, unsigned* count, unsigned* start, unsigned* end, std::string service_name)
{
    soci::session sql(*connectionPool);

    try
        {
            sql.begin();

            // Update beat
            soci::statement stmt1 = (
                                        sql.prepare << "INSERT INTO t_hosts (hostname, beat, service_name) VALUES (:host, UTC_TIMESTAMP(), :service_name) "
                                        "  ON DUPLICATE KEY UPDATE beat = UTC_TIMESTAMP()",
                                        soci::use(hostname), soci::use(service_name));
            stmt1.execute(true);

            // Total number of working instances
            soci::statement stmt2 = (
                                        sql.prepare << "SELECT COUNT(hostname) FROM t_hosts "
                                        "  WHERE beat >= DATE_SUB(UTC_TIMESTAMP(), interval 2 minute) and service_name = :service_name",
                                        soci::use(service_name),
                                        soci::into(*count));
            stmt2.execute(true);

            // This instance index
            // Mind that MySQL does not have rownum
            soci::rowset<std::string> rsHosts = (sql.prepare <<
                                                 "SELECT hostname FROM t_hosts "
                                                 "WHERE beat >= DATE_SUB(UTC_TIMESTAMP(), interval 2 minute) and service_name = :service_name "
                                                 "ORDER BY hostname",
                                                 soci::use(service_name)
                                                );

            soci::rowset<std::string>::const_iterator i;
            for (*index = 0, i = rsHosts.begin(); i != rsHosts.end(); ++i, ++(*index))
                {
                    std::string& host = *i;
                    if (host == hostname)
                        break;
                }

            sql.commit();

            if(*count != 0)
                {
                    // Calculate start and end hash values
                    unsigned segsize = UINT16_MAX / *count;
                    unsigned segmod  = UINT16_MAX % *count;

                    *start = segsize * (*index);
                    *end   = segsize * (*index + 1) - 1;

                    // Last one take over what is left
                    if (*index == *count - 1)
                        *end += segmod + 1;

                    this->hashSegment.start = *start;
                    this->hashSegment.end   = *end;
                }

            // Delete old entries
            sql.begin();
            soci::statement stmt3 = (sql.prepare << "DELETE FROM t_hosts WHERE beat <= DATE_SUB(UTC_TIMESTAMP(), interval 1 week)");
            stmt3.execute(true);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}


void MySqlAPI::updateOptimizerEvolution(soci::session& sql, const std::string & source_hostname, const std::string & destination_hostname, int active, double throughput, double successRate, int buffer, int bandwidth)
{
    try
        {
            if(throughput > 0 && successRate > 0)
                {
                    double agrthroughput = 0.0;
                    soci::indicator ind = soci::i_ok;
                    sql << " select sum(throughput) from t_file where file_state='ACTIVE' and source_se=:source_se and dest_se=:dest_se and throughput > 0 ",
                        soci::use(source_hostname), soci::use(destination_hostname), soci::into(agrthroughput, ind);

                    if(ind == soci::i_ok && agrthroughput > 0)
                        {
                            sql.begin();
                            sql << " INSERT INTO t_optimizer_evolution (datetime, source_se, dest_se, active, throughput, filesize, buffer, nostreams, agrthroughput) "
                                " values(UTC_TIMESTAMP(), :source, :dest, :active, :throughput, :filesize, :buffer, :nostreams, :agrthroughput) ",
                                soci::use(source_hostname),
                                soci::use(destination_hostname),
                                soci::use(active),
                                soci::use(throughput),
                                soci::use(successRate),
                                soci::use(buffer),
                                soci::use(bandwidth),
                                soci::use(agrthroughput);
                            sql.commit();
                        }
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

void MySqlAPI::snapshot(const std::string & vo_name, const std::string & source_se_p, const std::string & dest_se_p, const std::string &, std::stringstream & result)
{
    soci::session sql(*connectionPool);

    std::string vo_name_local;
    std::string dest_se;
    std::string source_se;
    std::string reason;
    long long countReason = 0;
    long long active = 0;
    long long maxActive = 0;
    long long submitted = 0;
    double throughput = 0.0;
    double tx_duration = 0.0;
    double queuingTime = 0.0;
    std::string querySe = " SELECT DISTINCT source_se, dest_se FROM t_job WHERE job_finished is NULL ";

    time_t now = time(NULL);
    struct tm tTime;
    gmtime_r(&now, &tTime);

    soci::indicator isNull1 = soci::i_ok;
    soci::indicator isNull2 = soci::i_ok;
    soci::indicator isNull3 = soci::i_ok;
    soci::indicator isNull4 = soci::i_ok;
    soci::indicator isNull5 = soci::i_ok;

    soci::statement voStmt(sql);
    if(!vo_name.empty())
        {
            voStmt = (sql.prepare << "select distinct vo_name from t_job where job_finished is null AND vo_name = :vo_name",
                        soci::use(vo_name), soci::into(vo_name_local));
        }
    else
        {
            voStmt = (sql.prepare << "select distinct vo_name from t_job where job_finished is null",
                        soci::into(vo_name_local));
        }

    soci::statement pairsStmt(sql);
    pairsStmt.exchange(soci::into(source_se));
    pairsStmt.exchange(soci::into(dest_se));
    if(!source_se_p.empty())
        {
            source_se = source_se_p;
            pairsStmt.exchange(soci::use(source_se));
            querySe += " AND source_se = :source_se ";
        }

    if(!dest_se_p.empty())
        {
            dest_se = dest_se_p;
            pairsStmt.exchange(soci::use(dest_se));
            querySe += " AND dest_se = :dest_se ";
        }
    querySe += " AND vo_name= :vo_name";
    pairsStmt.exchange(soci::use(vo_name_local));
    pairsStmt.alloc();
    pairsStmt.prepare(querySe);
    pairsStmt.define_and_bind();

    try
        {
            soci::statement st1((sql.prepare << "select count(*) from t_file where "
                                 " file_state='ACTIVE' and vo_name=:vo_name_local and "
                                 " source_se=:source_se and dest_se=:dest_se",
                                 soci::use(vo_name_local),
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::into(active)));

            soci::statement st2((sql.prepare << "select active from t_optimize_active where "
                                 " source_se=:source_se and dest_se=:dest_se",
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::into(maxActive, isNull1)
                                ));

            soci::statement st3((sql.prepare << "select count(*) from t_file where "
                                 " file_state='SUBMITTED' and vo_name=:vo_name_local and "
                                 " source_se=:source_se and dest_se=:dest_se",
                                 soci::use(vo_name_local),
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::into(submitted)
                                ));

            soci::statement st4((sql.prepare << "select avg(throughput) from t_file where  "
                                 " source_se=:source_se and dest_se=:dest_se "
                                 " AND file_state='ACTIVE' OR (file_state='FINISHED' and  job_finished >= (UTC_TIMESTAMP() - interval '60' minute)) "
                                 " AND throughput <> 0 ",
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::into(throughput, isNull2)
                                ));

            soci::statement st5((sql.prepare << "select reason, count(reason) as c from t_file where "
                                 " (job_finished > (UTC_TIMESTAMP() - interval '60' minute)) "
                                 " AND file_state='FAILED' and "
                                 " source_se=:source_se and dest_se=:dest_se and vo_name =:vo_name_local   "
                                 " group by reason order by c desc limit 1",
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::use(vo_name_local),
                                 soci::into(reason, isNull3),
                                 soci::into(countReason)
                                ));

            soci::statement st6((sql.prepare << " select avg(tx_duration) from t_file where file_state='FINISHED'  "
                                 " AND source_se=:source_se and dest_se=:dest_se and vo_name =:vo_name_local AND (job_finished > (UTC_TIMESTAMP() - interval '60' minute)) ",
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::use(vo_name_local),
                                 soci::into(tx_duration, isNull4)
                                ));


            soci::statement st7((sql.prepare << "  select avg(TIMESTAMPDIFF(SECOND, t_job.submit_time, t_file.start_time))  from t_file, t_job "
                                 "  where t_job.job_id=t_file.job_id  "
                                 " AND t_file.source_se=:source_se and t_file.dest_se=:dest_se and t_job.vo_name =:vo_name_local "
                                 " AND t_job.job_finished is NULL and t_file.job_finished is NULL and start_time is not NULL order by start_time DESC ",
                                 soci::use(source_se),
                                 soci::use(dest_se),
                                 soci::use(vo_name_local),
                                 soci::into(queuingTime, isNull5)
                                ));

            voStmt.execute();
            while (voStmt.fetch());
                {
                    if(source_se_p.empty())
                        source_se = "";
                    if(dest_se_p.empty())
                        dest_se = "";

                    pairsStmt.execute();
                    while (pairsStmt.fetch())
                        {
                            active = 0;
                            maxActive = 0;
                            submitted = 0;
                            throughput = 0.0;

                            result << std::fixed << "VO: ";
                            result <<   vo_name_local;
                            result <<   "\n";

                            result <<   "Source endpoint: ";
                            result <<   source_se;
                            result <<   "\n";
                            result <<   "Destination endpoint: ";
                            result <<   dest_se;
                            result <<   "\n";

                            //get active for this pair and vo
                            st1.execute(true);
                            result <<   "Current active transfers: ";
                            result <<   active;
                            result <<   "\n";

                            //get max active for this pair no matter the vo
                            st2.execute(true);
                            result <<   "Max active transfers: ";
                            result <<   maxActive;
                            result <<   "\n";

                            //get submitted for this pair and vo
                            st3.execute(true);
                            result <<   "Queued files: ";
                            result <<   submitted;
                            result <<   "\n";

                            //weighted-average throughput last sample
                            st4.execute(true);
                            result <<   "Avg throughput: ";
                            result <<  std::setprecision(2) << throughput;
                            result <<   " MB/s\n";

                            //success rate the last 1h
                            soci::rowset<soci::row> rs = (sql.prepare << "SELECT file_state FROM t_file "
                                                          "WHERE "
                                                          "      t_file.source_se = :source AND t_file.dest_se = :dst AND "
                                                          "      (t_file.job_finished > (UTC_TIMESTAMP() - interval '60' minute)) AND "
                                                          "      file_state IN ('FAILED','FINISHED') and vo_name = :vo_name_local ",
                                                          soci::use(source_se), soci::use(dest_se),soci::use(vo_name_local));


                            double nFailedLastHour = 0.0;
                            double nFinishedLastHour = 0.0;
                            double ratioSuccessFailure = 0.0;
                            for (soci::rowset<soci::row>::const_iterator i = rs.begin();
                                    i != rs.end(); ++i)
                                {
                                    std::string state = i->get<std::string>("file_state", "");

                                    if (state.compare("FAILED") == 0)
                                        {
                                            nFailedLastHour+=1.0;
                                        }
                                    else if (state.compare("FINISHED") == 0)
                                        {
                                            nFinishedLastHour+=1.0;
                                        }
                                }

                            //round up efficiency
                            if(nFinishedLastHour > 0.0)
                                {
                                    ratioSuccessFailure = ceil(nFinishedLastHour/(nFinishedLastHour + nFailedLastHour) * (100.0/1.0));
                                }

                            result <<   "Link efficiency (last hour): ";
                            result <<   long(ratioSuccessFailure);
                            result <<   "%\n";

                            //average transfer duration the last 30min
                            tx_duration = 0.0;
                            st6.execute(true);
                            result <<   "Avg transfer duration (last hour): ";
                            result <<   long(tx_duration);
                            result <<   " secs\n";

                            //average queuing time expressed in secs
                            queuingTime = 0;
                            st7.execute(true);
                            result <<   "Avg queuing time: ";
                            result <<   long(queuingTime);
                            result <<   " secs\n";

                            //most frequent error and number the last 30min
                            reason = "";
                            countReason = 0;
                            st5.execute(true);

                            result <<   "Most frequent error (last hour): ";
                            result <<   countReason;
                            result <<   " times: ";
                            result <<   reason;
                            result <<   "\n";

                            //get bandwidth restrictions (if any)
                            result << getBandwidthLimitInternal(sql, source_se, dest_se);

                            result << "\n\n";
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}


bool MySqlAPI::getDrain()
{
    soci::session sql(*connectionPool);

    try
        {
            return getDrainInternal(sql);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

bool MySqlAPI::getDrainInternal(soci::session& sql)
{

    int drain = 0;

    try
        {
            soci::indicator isNull = soci::i_ok;

            sql << "SELECT drain FROM t_hosts WHERE hostname = :hostname",soci::use(hostname), soci::into(drain, isNull);

            if(isNull == soci::i_null || drain == 0)
                return false;

            return true;
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
    return true;
}


void MySqlAPI::setDrain(bool drain)
{

    soci::session sql(*connectionPool);

    try
        {
            sql.begin();
            if(drain == true)
                sql << " update t_hosts set drain=1 where hostname = :hostname ",soci::use(hostname);
            else
                sql << " update t_hosts set drain=0 where hostname = :hostname ",soci::use(hostname);
            sql.commit();
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

std::string MySqlAPI::getBandwidthLimit()
{
    soci::session sql(*connectionPool);

    std::string result;

    try
        {
            std::string source_hostname;
            std::string destination_hostname;
            result = getBandwidthLimitInternal(sql, source_hostname, destination_hostname);
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return result;
}

std::string MySqlAPI::getBandwidthLimitInternal(soci::session& sql, const std::string & source_hostname, const std::string & destination_hostname)
{

    std::ostringstream result;

    try
        {
            if(!source_hostname.empty())
                {
                    double bandwidth = 0;
                    soci::indicator isNullBandwidth = soci::i_ok;
                    sql << " select throughput from t_optimize where source_se = :source_se and throughput is not NULL ",
                        soci::use(source_hostname), soci::into(bandwidth, isNullBandwidth);

                    if(sql.got_data() && bandwidth > 0)
                        result << "Source endpoint: " << source_hostname << "   Bandwidth restriction: " << bandwidth << " MB/s\n";
                }
            else if (!destination_hostname.empty())
                {
                    double bandwidth = 0;
                    soci::indicator isNullBandwidth = soci::i_ok;
                    sql << " select throughput from t_optimize where dest_se = :dest_se  and throughput is not NULL ",
                        soci::use(destination_hostname), soci::into(bandwidth, isNullBandwidth);

                    if(sql.got_data() && bandwidth > 0)
                        result << "Destination endpoint: " << destination_hostname << "   Bandwidth restriction: " << bandwidth << " MB/s\n";
                }
            else
                {
                    soci::rowset<soci::row> rs = (
                                                     sql.prepare <<
                                                     " SELECT source_se, dest_se, throughput from t_optimize "
                                                     " WHERE  "
                                                     " throughput is not NULL "
                                                 );

                    soci::rowset<soci::row>::const_iterator it;
                    for (it = rs.begin(); it != rs.end(); ++it)
                        {
                            std::string source_se = it->get<std::string>("source_se","");
                            std::string dest_se = it->get<std::string>("dest_se","");
                            double bandwidth = it->get<double>("throughput");

                            if(!source_se.empty() && bandwidth > 0)
                                {
                                    result << "Source endpoint: " << source_se << "   Bandwidth restriction: " << bandwidth << " MB/s\n";
                                }
                            if(!dest_se.empty() && bandwidth > 0)
                                {
                                    result << "Destination endpoint: " << dest_se   << "   Bandwidth restriction: " << bandwidth << " MB/s\n";
                                }
                        }
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return result.str();
}

void MySqlAPI::setBandwidthLimit(const std::string & source_hostname, const std::string & destination_hostname, int bandwidthLimit)
{

    soci::session sql(*connectionPool);

    try
        {
            long long int countSource = 0;
            long long int countDest = 0;

            if(!source_hostname.empty())
                {
                    sql << "select count(*) from t_optimize where source_se=:source_se  and throughput is not NULL ",
                        soci::use(source_hostname), soci::into(countSource);

                    if(countSource == 0 && bandwidthLimit > 0)
                        {
                            sql.begin();

                            sql << " insert into t_optimize(throughput, source_se) values(:throughput, :source_se) ",
                                soci::use(bandwidthLimit), soci::use(source_hostname);

                            sql.commit();
                        }
                    else if (countSource > 0)
                        {
                            if(bandwidthLimit == -1)
                                {
                                    sql.begin();
                                    sql << "update t_optimize set throughput=NULL where source_se=:source_se ",
                                        soci::use(source_hostname);
                                    sql.commit();
                                }
                            else
                                {
                                    sql.begin();
                                    sql << "update t_optimize set throughput=:throughput where source_se=:source_se and throughput is not NULL ",
                                        soci::use(bandwidthLimit), soci::use(source_hostname);
                                    sql.commit();
                                }
                        }
                }

            if(!destination_hostname.empty())
                {
                    sql << "select count(*) from t_optimize where dest_se=:dest_se and throughput is not NULL ",
                        soci::use(destination_hostname), soci::into(countDest);

                    if(countDest == 0 && bandwidthLimit > 0)
                        {
                            sql.begin();

                            sql << " insert into t_optimize(throughput, dest_se) values(:throughput, :dest_se) ",
                                soci::use(bandwidthLimit), soci::use(destination_hostname);

                            sql.commit();
                        }
                    else if (countDest > 0)
                        {
                            if(bandwidthLimit == -1)
                                {
                                    sql.begin();
                                    sql << "update t_optimize set throughput=NULL where dest_se=:dest_se ",
                                        soci::use(destination_hostname);
                                    sql.commit();
                                }
                            else
                                {
                                    sql.begin();
                                    sql << "update t_optimize set throughput=:throughput where dest_se=:dest_se  and throughput is not NULL ",
                                        soci::use(bandwidthLimit), soci::use(destination_hostname);
                                    sql.commit();
                                }

                        }
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }
}

bool MySqlAPI::isProtocolUDT(const std::string & source_hostname, const std::string & destination_hostname)
{
    soci::session sql(*connectionPool);

    try
        {
            soci::indicator isNullUDT = soci::i_ok;
            std::string udt;

            sql << " select udt from t_optimize where (source_se = :source_se OR source_se = :dest_se) and udt is not NULL ",
                soci::use(source_hostname), soci::use(destination_hostname), soci::into(udt, isNullUDT);

            if(sql.got_data() && udt == "on")
                return true;

        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return false;
}

int MySqlAPI::getStreamsOptimization(const std::string & source_hostname, const std::string & destination_hostname)
{
    soci::session sql(*connectionPool);

    try
        {
            long long int streamsFound = 0;
            soci::indicator isNullStreamsFound = soci::i_ok;

            sql << " select nostreams from t_optimize where "
                " source_se=:source_se and dest_se=:dest_se",
                soci::use(source_hostname), soci::use(destination_hostname), soci::into(streamsFound, isNullStreamsFound);

            if(sql.got_data() && streamsFound == -1)  //need to reduce num of streams, practically optimize
                {
                    return -1;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return 4;
}

int MySqlAPI::getGlobalTimeout()
{
    soci::session sql(*connectionPool);
    int timeout = 0;

    try
        {
            soci::indicator isNullTimeout = soci::i_ok;

            sql << " select global_timeout from t_server_config ", soci::into(timeout, isNullTimeout);

            if(sql.got_data() && timeout > 0)
                {
                    return timeout;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return timeout;

}

void MySqlAPI::setGlobalTimeout(int timeout)
{
    soci::session sql(*connectionPool);
    int timeoutLocal = 0;
    soci::indicator isNullTimeout = soci::i_ok;

    try
        {
            sql << "select global_timeout from t_server_config", soci::into(timeoutLocal, isNullTimeout);
            if (!sql.got_data())
                {
                    sql.begin();

                    sql << "INSERT INTO t_server_config (global_timeout) VALUES (:timeout) ",
                        soci::use(timeout);

                    sql.commit();

                }
            else
                {
                    sql.begin();

                    sql << "update t_server_config set global_timeout = :timeout",
                        soci::use(timeout);

                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

}

int MySqlAPI::getSecPerMb()
{
    soci::session sql(*connectionPool);
    int seconds = 0;

    try
        {
            soci::indicator isNullSeconds = soci::i_ok;

            sql << " select sec_per_mb from t_server_config ", soci::into(seconds, isNullSeconds);

            if(sql.got_data() && seconds > 0)
                {
                    return seconds;
                }
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception ");
        }

    return seconds;

}

void MySqlAPI::setSecPerMb(int seconds)
{
    soci::session sql(*connectionPool);
    int secondsLocal = 0;
    soci::indicator isNullSeconds = soci::i_ok;

    try
        {
            sql << "select sec_per_mb from t_server_config", soci::into(secondsLocal, isNullSeconds);
            if (!sql.got_data())
                {
                    sql.begin();

                    sql << "INSERT INTO t_server_config (sec_per_mb) VALUES (:seconds) ",
                        soci::use(seconds);

                    sql.commit();

                }
            else
                {
                    sql.begin();

                    sql << "update t_server_config set sec_per_mb = :seconds",
                        soci::use(seconds);

                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

}

void MySqlAPI::setSourceMaxActive(const std::string & source_hostname, int maxActive)
{
    soci::session sql(*connectionPool);
    std::string source_se;
    soci::indicator isNullSourceSe = soci::i_ok;

    try
        {
            sql << "select source_se from t_optimize where source_se = :source_se and active is not NULL", soci::use(source_hostname), soci::into(source_se, isNullSourceSe);

            if (!sql.got_data())
                {
                    if (maxActive > 0)
                        {
                            sql.begin();

                            sql << "INSERT INTO t_optimize (source_se, active) VALUES (:source_se, :active)  ",
                                soci::use(source_hostname), soci::use(maxActive);

                            sql.commit();
                        }
                }
            else
                {
                    sql.begin();

                    if (maxActive == -1)
                        {
                            sql << "update t_optimize set active = NULL where source_se = :source_se ",
                                soci::use(source_hostname);
                        }
                    else
                        {
                            sql << "update t_optimize set active = :active where source_se = :source_se and active is not NULL ",
                                soci::use(maxActive), soci::use(source_hostname);
                        }
                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }

}

void MySqlAPI::setDestMaxActive(const std::string & destination_hostname, int maxActive)
{
    soci::session sql(*connectionPool);
    std::string dest_se;
    soci::indicator isNullDestSe = soci::i_ok;

    try
        {
            sql << "select dest_se from t_optimize where dest_se = :dest_se and active is not NULL", soci::use(destination_hostname), soci::into(dest_se, isNullDestSe);

            if (!sql.got_data())
                {
                    if (maxActive > 0)
                        {
                            sql.begin();

                            sql << "INSERT INTO t_optimize (dest_se, active) VALUES (:dest_se, :active)  ",
                                soci::use(destination_hostname), soci::use(maxActive);

                            sql.commit();
                        }
                }
            else
                {
                    sql.begin();

                    if (maxActive == -1)
                        {
                            sql << "update t_optimize set active = NULL where dest_se = :dest_se ",
                                soci::use(destination_hostname);
                        }
                    else
                        {
                            sql << "update t_optimize set active = :active where dest_se = :dest_se  and active is not NULL ",
                                soci::use(maxActive), soci::use(destination_hostname);
                        }
                    sql.commit();
                }
        }
    catch (std::exception& e)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " + e.what());
        }
    catch (...)
        {
            sql.rollback();
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}

int MySqlAPI::getBufferOptimization()
{
    soci::session sql(*connectionPool);

    try
        {
            std::string name;
            soci::indicator isNullName = soci::i_ok;

            sql << "SELECT name FROM t_se WHERE name = 'buffer'", soci::into(name, isNullName);

            if (sql.got_data())
                {
                    return 1; //buffer optimization
                }
            return 	0; //default
        }
    catch (std::exception& e)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " +  e.what());
        }
    catch (...)
        {
            throw Err_Custom(std::string(__func__) + ": Caught exception " );
        }
}



// the class factories

extern "C" GenericDbIfce* create()
{
    return new MySqlAPI;
}

extern "C" void destroy(GenericDbIfce* p)
{
    if (p)
        delete p;
}

extern "C" MonitoringDbIfce* create_monitoring()
{
    return new MySqlMonitoring;
}

extern "C" void destroy_monitoring(MonitoringDbIfce* p)
{
    if (p)
        delete p;
}
