/*
*
*    Copyright (C) Max <max1976@mail.ru>
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include <iostream>
#include <Poco/NumberParser.h>
#include "main.h"
#include "nfqstatistictask.h"
#include "qdpi.h"
#include "AhoCorasickPlus.h"
#include "sendertask.h"
#include "nfqthread.h"

Poco::Mutex nfqFilter::_domainMapMutex;
DomainsMap nfqFilter::_domainsMap;
DomainsMap nfqFilter::_domainsUrlsMap;
DomainsMap nfqFilter::_domainsSSLMap;

IPPortMap nfqFilter::_ipportMap;
SSLIps    nfqFilter::_sslIpsSet;

Poco::Mutex nfqFilter::_urlMapMutex;

Poco::Mutex nfqFilter::_sslMutex;

struct ndpi_detection_module_struct* nfqFilter::my_ndpi_struct = NULL;
u_int32_t nfqFilter::ndpi_size_flow_struct = 0;
u_int32_t nfqFilter::ndpi_size_id_struct = 0;

u_int32_t nfqFilter::current_ndpi_memory = 0;
u_int32_t nfqFilter::max_ndpi_memory = 0;

AhoCorasickPlus *nfqFilter::atm=NULL;

AhoCorasickPlus *nfqFilter::atm_ssl=NULL;

AhoCorasickPlus *nfqFilter::atm_domains=NULL;

std::map<std::string, ADD_P_TYPES> add_type_s;

nfqFilter::nfqFilter():
	_helpRequested(false),
	_errorHandler(*this),
	_cmd_queueNum(-1),
	_cmd_threadsNum(-1)
{
	Poco::ErrorHandler::set(&_errorHandler);
}

nfqFilter::~nfqFilter()
{
}

void nfqFilter::initialize(Application& self)
{
	loadConfiguration();
	ServerApplication::initialize(self);

	if(_cmd_queueNum > 0)
	{
		_config.queueNumber=_cmd_queueNum;
	} else {
		_config.queueNumber=config().getInt("queue",0);
	}
	_config.max_pending_packets=config().getInt("max_pending_packets",DEFAULT_MAX_PENDING_PACKETS);
	_config.send_rst=config().getBool("send_rst", false);
	_config.mark_value=config().getInt("mark_value",MARK_VALUE);
	_config.block_undetected_ssl=config().getBool("block_undetected_ssl",false);
	_config.save_exception_dump=config().getBool("save_bad_packets",false);
	_config.lower_host=config().getBool("lower_host",false);
	_config.match_host_exactly=config().getBool("match_host_exactly",false);
	_config.url_decode=config().getBool("url_decode",false);

	if(_cmd_threadsNum > 0 && _cmd_threadsNum <= 16)
	{
		_config.num_threads=_cmd_threadsNum;
	} else {
		_config.num_threads=config().getInt("num_threads",2);
		if(_config.num_threads > 16)
			_config.num_threads=16;
	}

	std::string add_p_type=config().getString("url_additional_info","none");
	std::transform(add_p_type.begin(), add_p_type.end(), add_p_type.begin(), ::tolower);

	add_type_s["none"]=A_TYPE_NONE;
	add_type_s["line"]=A_TYPE_ID;
	add_type_s["url"]=A_TYPE_URL;

	std::map<std::string, ADD_P_TYPES>::iterator it=add_type_s.find(add_p_type);
	if(it == add_type_s.end())
	{
		throw Poco::Exception("Unknown url_additional_info type '" + add_p_type + "'",404);
	}
	_config.add_p_type=it->second;
	logger().debug("URL additional info set to %s", add_p_type);

	std::string http_code=config().getString("http_code","");
	if(!http_code.empty())
	{
		http_code.erase(std::remove(http_code.begin(), http_code.end(), '"'), http_code.end());
		_sender_params.code=http_code;
		logger().debug("HTTP code set to %s", http_code);
	}

	_domainsFile=config().getString("domainlist","");
	_urlsFile=config().getString("urllist","");
	_sender_params.redirect_url=config().getString("redirect_url","");
	_protocolsFile=config().getString("protocols","");

	std::string _sslFile=config().getString("ssllist","");
	_statistic_interval=config().getInt("statistic_interval",0);

	std::string _sslIpsFile=config().getString("sslips","");


	std::string _hostsFile=config().getString("hostlist","");

	logger().information("Starting up on queue: %d",_config.queueNumber);

	atm_domains=new AhoCorasickPlus();
	
	// читаем файл с доменами
	Poco::FileInputStream df(_domainsFile);
	if(df.good())
	{
		int lineno=1;
		while(!df.eof())
		{
			std::string str;
			getline(df,str);
			if(!str.empty())
			{
				AhoCorasickPlus::EnumReturnStatus status;
				AhoCorasickPlus::PatternId patId = lineno;
				status = atm_domains->addPattern(str, patId);
				if (status!=AhoCorasickPlus::RETURNSTATUS_SUCCESS)
				{
					if(status == AhoCorasickPlus::RETURNSTATUS_DUPLICATE_PATTERN)
					{
						logger().warning("Pattern %s already present in domains list",str);
					} else {
						logger().error("Failed to add %s from line %d",str,lineno);
					}
				} else {
					std::pair<DomainsMap::Iterator,bool> res=_domainsMap.insert(DomainsMap::ValueType(lineno,str));
					if(res.second)
					{
						logger().debug("Inserted domain: " + str + " from line %d",lineno);
					} else {
						logger().debug("Updated domain: " + str + " from line %d",lineno);
					}
				}
			}
			lineno++;
		}
	} else
		throw Poco::OpenFileException(_domainsFile);
	df.close();

	atm_domains->finalize();

	atm=new AhoCorasickPlus();

	atm_ssl=new AhoCorasickPlus();

	// читаем файл с url
	Poco::FileInputStream uf(_urlsFile);
	if(uf.good())
	{
		int lineno=1;
		while(!uf.eof())
		{
			std::string str;
			getline(uf,str);
			if(!str.empty())
			{
				AhoCorasickPlus::EnumReturnStatus status;
				AhoCorasickPlus::PatternId patId = lineno;
				status = atm->addPattern(str, patId);
				if (status!=AhoCorasickPlus::RETURNSTATUS_SUCCESS)
				{
					if(status == AhoCorasickPlus::RETURNSTATUS_DUPLICATE_PATTERN)
					{
						logger().warning("Pattern %s already present in URL database",str);
					} else {
						logger().error("Failed to add %s from line %d",str,lineno);
					}
				} else {
					std::size_t pos = str.find("/");
					if(pos != std::string::npos)
					{
						std::string host = str.substr(0,pos);
						std::pair<DomainsMap::Iterator,bool> res=_domainsUrlsMap.insert(DomainsMap::ValueType(lineno,host));
						if(res.second)
						{
							logger().debug("Inserted domain: " + str + " from line %d",lineno);
						} else {
							logger().debug("Updated domain: " + str + " from line %d",lineno);
						}
					} else {
						logger().fatal("Bad url format in line %d",lineno);
						throw Poco::Exception("Bad url format");
					}

				}
			}
			lineno++;
		}
	} else
		throw Poco::OpenFileException(_urlsFile);
	uf.close();


	atm->finalize();


	if(!_sslFile.empty())
	{
		// читаем файл с ssl hosts
		Poco::FileInputStream sslf(_sslFile);
		if(sslf.good())
		{
			int lineno=1;
			while(!sslf.eof())
			{
				std::string str;
				getline(sslf,str);
				if(!str.empty())
				{
					AhoCorasickPlus::EnumReturnStatus status;
					AhoCorasickPlus::PatternId patId = lineno;
					status = atm_ssl->addPattern(str, patId);
					if (status!=AhoCorasickPlus::RETURNSTATUS_SUCCESS)
					{
						if(status == AhoCorasickPlus::RETURNSTATUS_DUPLICATE_PATTERN)
						{
							logger().warning("Pattern %s already present in SSL database",str);
						} else {
							logger().error("Failed to add %s from line %d",str,lineno);
						}
					} else {
						std::pair<DomainsMap::Iterator,bool> res=_domainsSSLMap.insert(DomainsMap::ValueType(lineno,str));
						if(res.second)
						{
							logger().debug("Inserted domain: " + str + " from line %d",lineno);
						} else {
							logger().debug("Updated domain: " + str + " from line %d",lineno);
						}
					}
				}
				lineno++;
			}
		} else
			throw Poco::OpenFileException(_sslFile);
		sslf.close();
	}

	atm_ssl->finalize();

	if(!_hostsFile.empty())
	{
		// читаем файл с hosts
		Poco::FileInputStream hf(_hostsFile);
		if(hf.good())
		{
			int lineno=1;
			while(!hf.eof())
			{
				std::string str;
				getline(hf,str);
				if(!str.empty())
				{
					std::size_t found=str.find(":");
					std::string ip=str.substr(0, found);
					std::string port;
					unsigned short porti=0;
					if(found != std::string::npos)
					{
						port=str.substr(found+1,str.length());
						logger().debug("IP is %s port %s",ip,port);
						porti=atoi(port.c_str());
					} else {
						logger().debug("IP %s without port", ip);
					}
					Poco::Net::IPAddress ip_addr(ip);
					IPPortMap::iterator it=_ipportMap.find(ip_addr);
					if(it == _ipportMap.end())
					{
						std::set<unsigned short> ports;
						if(porti)
						{
							logger().debug("Adding port %s to ip %s", port, ip);
							ports.insert(porti);
						}
						_ipportMap.insert(std::make_pair(ip_addr,ports));
						logger().debug("Inserted ip: %s from line %d", ip, lineno);
					} else {
						logger().debug("Adding port %s from line %d to ip %s", port,lineno,ip);
						it->second.insert(porti);
					}
					
				}
				lineno++;
			}
		} else
			throw Poco::OpenFileException(_hostsFile);
		hf.close();
	}
	if(!_sslIpsFile.empty())
	{
		Poco::FileInputStream hf(_sslIpsFile);
		if(hf.good())
		{
			int lineno=1;
			while(!hf.eof())
			{
				std::string str;
				getline(hf,str);
				if(!str.empty())
				{
					std::pair<SSLIps::iterator,bool> ret;
					Poco::Net::IPAddress ip_addr(str);
					ret=_sslIpsSet.insert(ip_addr);
					if(ret.second == false)
					{
						logger().information("IP address %s already present at ssl ips list", ip_addr.toString());
					}
				}
				lineno++;
			}
		} else
			throw Poco::OpenFileException(_hostsFile);
		hf.close();
	}


	my_ndpi_struct = init_ndpi();

	if (my_ndpi_struct == NULL) {
		logger().error("Can't load nDPI!");
		// TODO вставить отключение ndpi
	}
	if(!_protocolsFile.empty())
	{
		ndpi_load_protocols_file(my_ndpi_struct, (char *)_protocolsFile.c_str());
	}
//	my_ndpi_struct->http_dissect_response=1; // не работает, т.к. у нас нет ответных пакетов от серверов...

	// Load sizes of main parsing structures
	ndpi_size_id_struct   = ndpi_detection_get_sizeof_ndpi_id_struct();
	ndpi_size_flow_struct = ndpi_detection_get_sizeof_ndpi_flow_struct();
}

void nfqFilter::uninitialize()
{
	logger().debug("Shutting down");
	ServerApplication::uninitialize();
}

void nfqFilter::defineOptions(Poco::Util::OptionSet& options)
{
	ServerApplication::defineOptions(options);
	options.addOption(
		Poco::Util::Option("help","h","display help on command line arguments")
			.required(false)
			.repeatable(false)
			.callback(Poco::Util::OptionCallback<nfqFilter>(this,&nfqFilter::handleHelp)));
	options.addOption(
		Poco::Util::Option("config_file","c","specify config file to read")
			.required(true)
			.repeatable(false)
			.argument("file"));
	options.addOption(
		Poco::Util::Option("queue","q","specify queue number")
			.required(false)
			.repeatable(false)
			.argument("queue_num"));

	options.addOption(
		Poco::Util::Option("threads","t","specify number of running threads")
			.required(false)
			.repeatable(false)
			.argument("threads_num"));

}

void nfqFilter::handleOption(const std::string& name,const std::string& value)
{
	ServerApplication::handleOption(name, value);
	if(name == "config_file")
	{
		loadConfiguration(value);
	}
	if(name == "queue")
	{
		_cmd_queueNum = Poco::NumberParser::parse(value);
	}
	if(name == "threads")
	{
		_cmd_threadsNum = Poco::NumberParser::parse(value);
	}
}

void nfqFilter::handleHelp(const std::string& name,const std::string& value)
{
	_helpRequested=true;
	displayHelp();
	stopOptionsProcessing();
}

void nfqFilter::displayHelp()
{
	Poco::Util::HelpFormatter helpFormatter(options());
	helpFormatter.setCommand(commandName());
	helpFormatter.setUsage("<-c config file> [options]");
	helpFormatter.setHeader("NfQueue filter");
	helpFormatter.format(std::cout);
}

int nfqFilter::main(const ArgVec& args)
{
	if(!_helpRequested)
	{
		Poco::TaskManager tm;
		tm.start(new NFQStatisticTask(_statistic_interval));
		tm.start(new nfqThread(_config));
		tm.start(new SenderTask(_sender_params));
		waitForTerminationRequest();
		tm.cancelAll();
		SenderTask::queue.wakeUpAll();
		tm.joinAll();
	}
	return Poco::Util::Application::EXIT_OK;
}





POCO_SERVER_MAIN(nfqFilter)
