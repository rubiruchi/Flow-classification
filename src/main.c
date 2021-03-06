#include <stdio.h>

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/errno.h>
#include <string.h>
#include <getopt.h>

#include <pthread.h>
#include <signal.h>

#include "common.h"

#include "unix_socket.h"
#include "json_handler.h"

#include "report_thread.h"
#include "polic_thread.h"

#include "handler_packet.h"
#include "daemon.h"

#define DEFAULT_CONFIG_FILE	"/etc/flow_cfg.json"

#define PID_FILE	"/tmp/flow-pid.pid"


//char *log_path="/tmp/dlp.log";					//用于发送报警信息的RabbitMQ
char *cmd_path="/tmp/flow_cmd.sock";					//用于发送报警信息的RabbitMQ

typedef struct _thread_info_t
{
	int usock;
}thread_info_t;

char polic_ip[32] = {0};
char report_ip[32] = {0};
int  polic_port = 0;
int  report_port = 0;
char device[32] = {0};

net_buff_queue_t *g_net_queue;
prog_info_t		 *g_info = NULL;


int g_debug =0;

static void print_help()
{
	printf("--usage:--\n");
	printf("-- build time:%s--%s\n",__TIME__,__DATE__);
	printf("-c config file\n");
	printf("-p polic file\n");
	printf("-n no daemon\n");
	printf("-m allow multi-worker\n");
	printf("-d debug mode\n");
	printf("-h print help\n");
}

int main(int argc,char *argv[])
{
	//init pararms 读取配置文件
	char config_file[64] = {0};
	char polic_file[64] = {0};
	int daemon = 1;		//默认daemon运行且单实例
	int only_one = 1;
	int opt;
	while((opt = getopt(argc,argv,"c:p:ndmh")) != -1 ){
		switch(opt){
			case 'c':
				strcpy(config_file,optarg);
				break;
			case 'p':
				strcpy(polic_file,optarg);
				break;
			case 'n':
				daemon = 0;
				break;
			case 'm':
				only_one = 0;
				break;
			case 'd':
				g_debug = 1;
				//daemon = 0;
				//only_one = 0;
				break;
			case 'h':
				print_help();
				return 1;
			case '?':
				print_help();
				return 1;
			default:
				break;
		}
	}

	signal(SIGPIPE,SIG_IGN);


#if 1
	JSON_INFO *cfg_info = NULL;
	if(strlen(config_file) > 2)
		cfg_info = json_ParseFile(config_file);
	else
		cfg_info = json_ParseFile(DEFAULT_CONFIG_FILE);
		
	if(cfg_info != NULL)
	{

		strcpy(polic_ip,json_getString(cfg_info,"polic_ip"));	
		strcpy(report_ip,json_getString(cfg_info,"report_ip"));	
		move_string_common(polic_ip);
		move_string_common(report_ip);

		char port[16];

		strcpy(port,json_getString(cfg_info,"polic_port"));
		move_string_common(port);
		polic_port = atoi(port);

		strcpy(port,json_getString(cfg_info,"report_port"));
		move_string_common(port);
		report_port = atoi(port);
		
		strcpy(device,json_getString(cfg_info,"device"));	
		move_string_common(device);
	}
	else
	{
		fprintf(stderr,"parse Config File Error.\n");
		exit(-1);
	}
	if(1)
	{
		printf("Prgress run with config:\n");
		printf("# polic  address : %s:%d\n",polic_ip,polic_port);
		printf("# report address : %s:%d\n",report_ip,report_port);
	}

	
	json_Delete(cfg_info);

#endif
	if(daemon){
		daemonize_user();
	}

	if(only_one){
		if(already_running(PID_FILE)){
			fprintf(stderr,"The Process [%s] already running .\n",argv[0]);
			exit(-1);
		}
	}



	//创建一个全局的数据结构用于保存抓包策略
	g_info = (prog_info_t *)malloc(sizeof(prog_info_t));
	if(g_info == NULL)
	{
		fprintf(stderr,"Init global info error.\n");
		exit(-1);
	}
	g_info->isset = 0;
	g_info->updata_flag = 0;
	memset(g_info->bpf,'\0',MAX_BPF_LEN);
	MUTEX_SETUP(g_info->lock);


	//创建一个全局的队列用于接收和发送buff
	g_net_queue  = net_new_queue(20);
	if(g_net_queue == NULL)
	{
		fprintf(stderr,"Create global queue Error\n");
		exit(-1);
	}
	

	if(1)	//创建一个线程用于接受策略
	{
		pthread_t polic_tid;
		p_thread_info_t *p_info;
		p_info = (p_thread_info_t *)malloc(sizeof(p_thread_info_t));
		strcpy(p_info->polic_ip,polic_ip);
		p_info->polic_port=polic_port;
		if(pthread_create(&polic_tid,NULL,polic_thread,(void *)p_info))
		{
			if(g_debug)
				perror("[ERROR] pthread create config Fail.");
		}
	}

	//等待接收到配置策略
	while(1)
	{
		if(prase_polic_file(polic_file) == 0)
			break;
		sleep(5);
		if(g_debug)
			printf("wait_polic--->\n");
		if(check_polic_ready())
			break;
	}

	
	if(1)	// 创建一个线程用于发送数据包
	{
		pthread_t report_tid;
		r_thread_info_t *r_info;
		r_info = (r_thread_info_t *)malloc(sizeof(r_thread_info_t));
		strcpy(r_info->report_ip,report_ip);
		r_info->report_port=report_port;

		if(pthread_create(&report_tid,NULL,report_thread,(void *)r_info))
		{
			if(g_debug){
				perror("[ERROR] pthread create report Fail.");
			}
		}

	}

	int ret;
	char buff[2048];
	//server_fds_t serverfds; 

	int cmd_sock = 0;

	int new_sock;

	cmd_sock = create_unixsocket_listener(cmd_path);
	//bro_sock = create_unixsocket_listener(bro_path);

	if(cmd_sock < 0)
	{
		fprintf(stderr,"create config listener error.\n");
		exit(-1);
	}

	net_buff_t *tmp = NULL;
	int buff_size = 1024;

	char polic[512] = {0};

	while(1)
	{
		sleep(2);
		if(g_debug)
			printf("[Main Thread]----------------->\n");
		/*
		tmp = new_net_buff(buff_size);
		if(tmp != NULL)
		{
			sprintf(tmp->buff," I hava buffsize [%d]",buff_size);
			buff_size+=16;
			if(net_add_buff(g_net_queue,tmp) != 0)
			{
				fprintf(stderr,"[ERROR] Add buff to queue Fail\n");
			}
		}
		*/
		get_polic(polic);
		if(g_debug) {
			printf("[MAIN] capture loop get polic [%s]\n",polic);
		}
		mt_pcap_capture(device,polic);

		
	}
	
}
