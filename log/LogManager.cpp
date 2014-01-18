#include <algorithm>
#include <fstream>
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/log/expressions/predicates/is_in_range.hpp>
#include <boost/log/utility/setup/from_settings.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/attributes/constant.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#include <boost/log/utility/setup/filter_parser.hpp>
#include "common/FileHandler.h"
#include  "log/LogManager.h"
using namespace boost::xpressive;

BOOST_LOG_ATTRIBUTE_KEYWORD(logger_id,"LoggerId",uint64_t)

    LogManager::LogManager()
    :mFileId(0)
{
    uint64_t multiple = 1;
    uint64_t stepMulti = 1 << LoggerInfo::LEVEL_BITS;
    for(int i = 0; i < LoggerInfo::MAX_LEVEL; ++i)
    {
        mIds[i] = 1;
        mIdMultiple[LoggerInfo::MAX_LEVEL - i - 1] = multiple;
        multiple *= stepMulti;
    }

    logging::register_simple_formatter_factory<SeverityLevel,char>("Severity");
    logging::register_simple_formatter_factory<uint64_t,char>("LoggerId");

    logging::register_simple_filter_factory<SeverityLevel, char >("Severity");
    logging::register_simple_filter_factory<uint64_t,char>("LoggerId");
}

LOG_EXPORT bool LogManager::Initialize(const std::string& strPath)
{
    PathVec configs;
    auto itLast = std::find_if(strPath.begin(),strPath.end(),[](char c){ return '*' == c || '?' == c;});
    fs::path fullPath = fs::system_complete(fs::path(std::string(strPath.begin(),itLast)));

    auto printError = [&strPath](){std::cerr <<__FILE__<<":"<<__LINE__<<":"<<strPath<<" is not a valid log configure file"<<std::endl;};
    if(!fs::exists(fullPath))
    {
        printError(); 
        return false;
    }

    if(fs::is_regular_file(fullPath))
    {
        configs.push_back(fullPath);
    }
    else if(fs::is_directory(fullPath))
    {
        const auto& pattern = Wildcard2Regex(strPath);
        FindFiles(fullPath,pattern,configs);

    }
    else
    {
        printError();
        return false;
    }

    if(0 == configs.size())
    {
        printError();
        return false;
    }
    else
    {
        std::vector<LoggerInfo*> loggers;
        for(auto config : configs)
        {
            LoadConfig(config,loggers);
        }
        LinkLoggerWithSink(loggers);                
    }

    return true;
}

void LogManager::LoadConfig(const fs::path& path,std::vector<LoggerInfo*>& loggers)
{
    std::ifstream settingFile(path.string().c_str());
    if(settingFile.is_open())
    {
        ++mFileId;
        auto setts = logging::parse_settings(settingFile);

        // Apply core settings
        if (section core_params = setts["Core"])
            _apply_core_settings(core_params);

        // Construct and initialize sinks
        if (section sink_params = setts["Sinks"])
        {
            for (section::const_iterator it = sink_params.begin(), end = sink_params.end(); it != end; ++it)
            {
                section sink_params = *it;

                // Ignore empty sections as they are most likely individual parameters (which should not be here anyway)
                if (!sink_params.empty())
                {
                    auto pSink = boost::static_pointer_cast<sinks::basic_sink_frontend>(construct_sink_from_settings(sink_params));
                    auto pSinkInfo = mSinkInfoPool.construct(pSink);
                    auto filter = sink_params["Filter"].get();
                    if(filter)
                    {
                        pSinkInfo->InitFilter = logging::parse_filter(filter.get());
                    }
                    pSink->set_filter(boost::bind(&SinkInfo::FiltFun,pSinkInfo,_1));
                    mSinks.insert(std::make_pair(it.get_name() + boost::lexical_cast<std::string>(mFileId),pSinkInfo));
                }
            }

            std::for_each(mSinks.begin(), mSinks.end()
                , [](boost::unordered_map<std::string,SinkInfo*>::value_type sinkPair)
            { 
                logging::core::get()->add_sink(sinkPair.second->Sink);
            });
        }

        if(section::reference logger_parms = setts["Loggers"])
        {
            LoadLogger(logger_parms,-1,loggers,"",0);
        }
    }
    else
    {
        std::cerr << __FILE__ <<":"<<__LINE__<<":"<<"open log configure file failed."<<std::endl;
    }
}

void LogManager::LoadLogger(section::reference& rSection,uint8_t nLevel,std::vector<LoggerInfo*>& loggers,const std::string& strName,uint64_t parentId)
{
    auto value = rSection.get();
    section sec = rSection;
    if(1 == sec.property_tree().count("Sinks"))
    {
        LoggerInfo* pInfo = nullptr;
        if(strName == "Root")
        {
            pInfo = RootInfo();
        }
        else
        {
            pInfo = new LoggerInfo();
        }
        pInfo->Id = GetId(strName,nLevel,parentId);
        pInfo->FileId = mFileId;
        pInfo->Level = nLevel;

        auto opSinkNames = sec["Sinks"].get();
        if(opSinkNames)
        {
            pInfo->SinkNames = opSinkNames.get();
        }

        auto filter = sec["Filter"].get();
        if(filter)
        {
            pInfo->Filter = logging::parse_filter(filter.get());
        }
        mLoggerInfos.put<LoggerInfo*>(strName,pInfo);
        loggers.push_back(pInfo);
    }
    else if(!sec.empty())
    {
        std::string childName = strName;
        if(0 != strName.length())
        {
            parentId = GetId(strName,nLevel,parentId);
            childName += ".";
        }

        for ( section::const_iterator it = sec.begin(), end = sec.end(); it != end; ++it)
        {
            section childSec = *it;
            if(!childSec.empty())
            {
                auto childRef = sec[it.get_name()];
                LoadLogger(childRef,nLevel + 1,loggers,childName + it.get_name(),parentId);
            }
        }
    }
}

uint64_t LogManager::GetId(const std::string& name,uint8_t level,uint64_t parentId)
{
    auto itId = mIdMap.find(name);
    if(itId != mIdMap.end())
    {
        return itId->second;
    }
    else
    {
        uint64_t id = mIds[level]++;
        id = (id * mIdMultiple[level]) + parentId;
        mIdMap[name] = id;
        return id;
    }
}

void LogManager::LinkLoggerWithSink(std::vector<LoggerInfo*>& loggers)
{
    for(auto pLogInfo : loggers)
    {
        std::vector<boost::iterator_range<std::string::iterator> > sinkNames;
        boost::split(sinkNames,pLogInfo->SinkNames,[](char c){ return ',' == c;},boost::token_compress_on);
        for(auto itSinkName : sinkNames)
        {
            auto itSink = mSinks.find(std::string(itSinkName.begin(),itSinkName.end()) + boost::lexical_cast<std::string>(pLogInfo->FileId));
            if(itSink != mSinks.end())
            {
                pLogInfo->SinkInfos.push_back(itSink->second);
            }
        }
    }
}

LOG_EXPORT  Logger& LogManager::GetLogger(const std::string& name)
{
    LoggerInfo* pInfo = GetLoggerInfo(name);
    if(nullptr != pInfo)
    {
        if(nullptr == pInfo->Log)
        {
            pInfo->Log = mLoggerPool.construct();
            pInfo->Log->add_attribute("LoggerId",attrs::constant<uint64_t>(pInfo->Id));

            for(auto pSink : pInfo->SinkInfos)
            {
                pSink->AddFilter(expr::is_in_range(logger_id,pInfo->Id,pInfo->NextId()),pInfo->Filter);
            }
        }

        return *pInfo->Log;
    }

    std::cerr << __FILE__ <<":"<<__LINE__<<":"<<"get logger "<<name << " failed."<<std::endl;
    throw std::runtime_error("get logger " + name + " failed");
}

LoggerInfo* LogManager::GetLoggerInfo(const std::string& name)
{
    using namespace std;
    size_t lastPos = name.size();
    while(string::npos != lastPos)
    {
        LoggerInfo* pInfo = mLoggerInfos.get<LoggerInfo*>(name.substr(0,lastPos),nullptr);
        if(nullptr != pInfo)
        {
            return pInfo;
        }
        lastPos = name.find_last_of('.');
    }

    return RootInfo();
}

LoggerInfo* LogManager::RootInfo( void )
{
    static LoggerInfo* rootInfo;
    if(nullptr == rootInfo)
    {
        rootInfo->Id = 0;
        rootInfo->Level = 0;
        rootInfo->Log = mLoggerPool.construct();
    }

    return rootInfo;
}


