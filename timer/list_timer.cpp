#include "list_timer.h"

int Utils::set_non_blocking(int fd)
{
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}

void Utils::add_fd(int epoll_fd,int fd,bool one_shot,bool ET_Mode)
{
	epoll_event event;
	event.data.fd = fd;

	if(ET_Mode == true)
	{
		//ET模式
		event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	}
	else
	{
		//LT模式
		event.events = EPOLLIN | EPOLLRDHUP;
	}

	//是否开启EPOLLONESHOT
	if(one_shot == true)
	{
		event.events |= EPOLLONESHOT;
	}

	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd,&event);
	set_non_blocking(fd);
}

void Utils::sig_handler(int sig)
{
	//为保证函数的可重入性，保留原来的errno
	//可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
	int save_errno = errno;
	int msg = sig;

	//将信号值从管道写端写入，传输字符类型，而非整型
	send(pipe_fd[1],(char*)&msg,1,0);

	//将原来的errno赋值为当前的errno
	errno = save_errno;
}

void Utils::add_sig(int sig,void(handler)(int),bool restart = true)
{
	//创建sigaction结构体变量
	struct sigaction sa;
	memset(&sa,'\0',sizeof(sa));

	//信号处理函数中仅仅发送信号值，不做对应逻辑处理
	sa.sa_handler = handler;
	if(restart)
	{
		//SA_RESTART，使被信号打断的系统调用自动重新发起
		sa.sa_flags |= SA_RESTART;
	}

	//将所有信号添加到信号集中
	sigfillset(&sa.sa_mask);

	//执行sigaction函数
	assert(sigaction(sig,&sa,NULL) != -1);
}

void Utils::timer_handler()
{
	timer_list.tick();
	alarm(TIMESLOT);
}

sort_timer_list::sort_timer_list():head(NULL),tail(NULL){}

sort_timer_list::~sort_timer_list()
{
	util_timer* tmp = head;
	while(tmp != NULL)
	{
		head = tmp->next;
		delete tmp;
		tmp = head;
	}
}

void sort_timer_list::add_timer(util_timer* timer)
{
	if(timer == NULL)
	{
		return;
	}
	if(head == NULL)
	{
		tail = timer;
		head = timer;
		return;
	}

	//如果新的定时器超时时间小于当前头部结点
	//直接将当前定时器结点作为头部结点
	if(timer->expire < head->expire)
	{
		timer->next = head;
		head->prev = timer;
		head = timer;
	}

	//否则调用私有成员，调整内部结点
	add_timer(timer,head);
}

void sort_timer_list::adjust_timer(util_timer* timer)
{
	if(timer == NULL)
	{
		return;
	}
	util_timer* tmp = timer->next;

	//被调整的定时器在链表尾部
	//定时器超时值仍然小于下一个定时器超时值，不调整
	if(tmp == NULL || (timer->expire < tmp->expire))
	{
		return;
	}
	//被调整定时器是链表头结点，将定时器取出，重新插入
	if(timer == head)
	{
		head = head->next;
		head->prev = NULL;
		timer->next = NULL;
		add_timer(timer);
	}
	else
	{
		//被调整定时器在内部，将定时器取出，重新插入
		timer->prev->next = timer->next;
		timer->next->prev = timer->prev;
		add_timer(timer);
	}
}

void sort_timer_list::del_timer(util_timer* timer)
{
	if(timer == NULL)
	{
		return;
	}

	//链表中只有一个定时器，需要删除该定时器
	if((timer == head) && (timer == tail))
	{
		delete timer;
		head = NULL;
		tail = NULL;
		return;
	}

	//被删除的定时器为头结点
	if(timer == head)
	{
		head = head->next;
		head->prev = NULL;
		delete timer;
		timer = NULL;
		return;
	}

	//被删除的定时器为尾结点
	if(timer == tail)
	{
		tail = tail->prev;
		tail->next = NULL;
		delete timer;
		timer = NULL;
		return;
	}

	//被删除的定时器在链表内部，常规链表结点删除
	timer->prev->next = timer->next;
	timer->next->prev = timer->prev;
	delete timer;
	timer = NULL;
	return;
}

void sort_timer_list::add_timer(util_timer* timer,util_timer* list_head)
{
	util_timer* prev = list_head;
	util_timer* tmp = prev->next;

	//遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
	while(tmp != NULL)
	{
		if(timer->expire < tmp->expire)
		{
			prev->next = timer;
			timer->next = tmp;
			tmp->prev = timer;
			timer->prev = prev;
			break;
		}
		prev = tmp;
		tmp = tmp->next;
	}

	//遍历完发现，目标定时器需要放到尾结点处
	if(tmp == NULL)
	{
		prev->next = timer;
		timer->prev = prev;
		timer->next = NULL;
		tail = timer;
	}
}

void sort_timer_list::tick()
{
	if(head == NULL)
	{
		return;
	}

	//获取当前时间
	time_t cur = time(NULL);
	util_timer* tmp = head;

	//遍历定时器链表
	while(tmp != NULL)
	{
		//链表容器为升序排列
		//当前时间小于定时器的超时时间，后面的定时器也没有到期
		if(cur < tmp->expire)
		{
			break;
		}

		//当前定时器到期，则调用回调函数，执行定时事件
		tmp->cb_func(tmp->user_data);

		//将处理后的定时器从链表容器中删除，并重置头结点
		head = tmp->next;
		if(head != NULL)
		{
			head->prev = NULL;
		}
		delete tmp;
		tmp = head;
	}
}

class Utils;
//定时器回调函数
void cb_func(client_data *user_data)
{
	//删除非活动连接在socket上的注册事件
	epoll_ctl(Utils::epoll_fd,EPOLL_CTL_DEL,user_data->sock_fd,0);
	assert(user_data);

	//关闭文件描述符
	close(user_data->sock_fd);

	//减少连接数
	--http_conn::user_count;
}