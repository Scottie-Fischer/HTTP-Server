//---------------------------------------------------//
//Multithreaded HTTP Server
//Scott Fischer
//Assignment 2
//---------------------------------------------------//

#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h> // write
#include <string.h> // memset
#include <stdlib.h> // atoi
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <math.h>
#include <sys/file.h>

//#include <stdio.h>       /* standard I/O routines               /
#include <pthread.h>     /* pthread functions and data structures */
#include <semaphore.h>
#define BUFFER_SIZE 1024

//Def for linked list
typedef struct Node{
	int thread_id;			//This will be used for storing the id 
	char* buffer;			//This will only be used for storing the log queues
	struct Node* next;
	int local_offset;
}Node;

//This is a 2D Linked List Object that we use for the logQueue 

typedef struct logObj{
	Node* queue[BUFFER_SIZE];
	int pos;
}logObj;

struct httpObj{
        //Create a struct to parse and sort HTTP request into sections
        char method[6];         //Holds Request Type: PUT or HEAD or GET
        char filename[30];      //Holds what the file we are trying to act on
        char httpversion[9];    //Holds that bit HTTP/#
        ssize_t contentlength;  //length of data we are given or pulling
        int statuscode;         //200,201,400,404,405,500
        uint8_t buffer[BUFFER_SIZE];
        Node* list;
	//char* header;
};

//This is the Argument to all threads that allows them to pass data between eachother
typedef struct threadArg{
	int log_offset;
	int log_flag;
	int ID;			//ID for each server
	int* queue;		//This holds the socket number of each unhandled request currently given to us
	sem_t request_sem;	//This stops busy waiting with the connector
	sem_t thread_sem;	//This stops the connector from assigning sockets to busy threads
	sem_t log_sem;		//This is to add atomicity to incrementing offset
	sem_t writer_sem;
	sem_t health_sem;

	Node* server_queue;	//Holds Server Thread that are currently waiting 
	int* socket_list;	//This is where the servers pull the sockets from
	sem_t *server_sems;	//This is the semaphore array that holds sems for each server 
	
	logObj log_queue;
	char* log_file;		//Holds file descriptor for log file
	int log_entries;
	int error_count;
}threadArg;


//
void printLog(Node *head){
        Node *temp = head;
        while(temp != NULL){
                printf("Thread: %s\n",temp->buffer);
                temp = temp->next;
        }
}

logObj addList(logObj logQueue, Node* list){
	int len = logQueue.pos;
	//printf("After here\n");
	//printLog(list);
	logQueue.queue[len] = list;
	return logQueue;
}

//---------------------------------------Node Functions----------------------------------------//
void printList(Node* head){
        Node *temp = head;
        while(temp != NULL){
                printf("PRINT LIST Node: %d\n",temp->thread_id);
                temp = temp->next;
        }       
}

Node* makeServerQueue(Node* head, int count){
	Node *temp;
	for(int i = 0; i < count; i++){
		temp = malloc(sizeof(struct Node));
		temp->thread_id = i;
		temp->next = head;
		head = temp;
	}
	return head;
}
Node* removeThread(Node* head){	
	Node *temp = head;
	head = head->next;
	free(temp);
	return head;
}
Node* insertThread(Node* head,int id){
	Node *new = malloc(sizeof(struct Node));
	new->thread_id = id;
	new->next = head;
	head = new;
	return head;
}

logObj removeList(logObj logQueue,Node* list){
	Node* temp = list;
        Node* holder = list;

	temp = temp->next;
	free(holder);
	if(temp != NULL){
        while(temp != NULL){
		holder = temp;
		temp = temp->next;
		free(holder);
        }
	}
        return logQueue;
}

Node* addLogTwo(Node *head, char* buffer, Node* new){
	if(head == NULL){
		head = new;
		head->buffer = buffer;
	}
	else{
		Node* holder;
		Node* temp = head;
		while(temp != NULL){
			holder = temp->next;
			if(holder == NULL){break;}
			temp = temp->next;
		}
		new->buffer = buffer;
		temp->next = new;
	}
	return head;
}

Node* addHeader(Node* head, char* buffer){
	Node* new = malloc(sizeof(struct Node));
	new->buffer = buffer;
	if(head != NULL){
		new->next = head;
	}else{
		new->next = NULL;
	}
	head = new;
	return head;
}

//----------------------------------Server Functions-----------------------------------------//
int fileCheck(struct httpObj* message){
        char* file = message->filename;
        for(size_t i = 0; i < strlen(file); i++){
                char c = file[i];
                if(! (( c >= 'A' && c <= 'Z') ||
                      ( c >= 'a' && c <= 'z') ||
                      ( c >= '0' && c <= '9') ||
                        c == '-' || c == '_')){return 1;}
        }
        return 0;
}
void errorCheck(struct httpObj* message){
        //Checking HTTP Version
        if(strcmp(message->httpversion,"HTTP/1.1") != 0){
                message->statuscode = 400;
        }
        //Check filename
        struct stat st;
	if(strcmp(message->filename,"/healthcheck") == 0 || strcmp(message->filename,"healthcheck") == 0){
		strcpy(message->filename,"healthcheck");
	}
	else if(sscanf(message->filename,"/%s",message->filename) != 1){
                if(strcmp(message->filename,"healthcheck") != 0){
			printf("%s is getting error code 400",message->filename);
			message->statuscode = 400;
		}
        }
        else if(fileCheck(message)){
                message->statuscode = 400;
        }
        else if(strlen(message->filename)>27){
                message->statuscode = 400;
        }
        else if(stat(message->filename,&st) != 0){
                message->statuscode = 404;
        }
        else if(access(message->filename,R_OK) != 0){
                message->statuscode = 403;
        }

}

void writeHealthCheck(int log_count, int error_count, struct httpObj* message, threadArg *Arg, int fd){
	
	char *healthcheck = malloc(BUFFER_SIZE * sizeof(char));
	char *errors = malloc(BUFFER_SIZE * sizeof(char));
	char* logs = malloc(BUFFER_SIZE * sizeof(char));
	char* cont = malloc(BUFFER_SIZE * sizeof(char));
	sem_wait(&Arg->health_sem);	
	sprintf(cont,"%d",message->contentlength);
	sprintf(errors,"%d",error_count);
	sprintf(logs,"%d",log_count);

	int contentlen = strlen(errors) + strlen(logs) + 1;
	
	sprintf(healthcheck,"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s\n%s",contentlen,errors,logs);
	write(fd,healthcheck,strlen(healthcheck));
	close(fd);
	sem_post(&Arg->health_sem);
}
//-------------------------------------------SERVER FUNCTIONS------------------------------------------------------------------//
void readHttpResponse(int client_sockd, struct httpObj* message){
        //This will parse the Request
        uint8_t header[BUFFER_SIZE];
        message->statuscode = 200;
        message->contentlength = 0;
        char* tok;
        int rcount;
        size_t scan;

        rcount = read(client_sockd,header,BUFFER_SIZE);
	if(rcount < 0){perror("");}
        tok = strtok(header,"\r\n");    //splits the header to get content length
        //Parse through the header, and assign each attribute of message accordingly
        scan = sscanf(tok,"%s %s %s",message->method,message->filename ,message->httpversion);  //scan for main header elements
        if(scan == 3){
                //Now scan for content length number
                while(tok != NULL){
                        if(sscanf(tok,"Content-Length: %ld", &(message->contentlength)) == 1){
                        }
                        tok = strtok(NULL,"\r\n");
                }
        }
        else{
                printf("%s is bad request\n",message->filename);
		message->statuscode = 400;      //Request is bad: wrong format
        }
        errorCheck(message);
        //Set Status Code
        if(strcmp(message->method,"PUT") == 0 && message->statuscode != 400){
                //Set the Status Code
                message->statuscode = 201;
        }
        else if(strcmp(message->method,"GET") == 0 && message->statuscode == 200){
                //Set Status Code
                message->statuscode = 200;
        }
        else if(strcmp(message->method,"HEAD") == 0 && message->statuscode == 200){
                //Set Status Code
                message->statuscode = 200;
        }
        else if(message->statuscode != 404 && message->statuscode != 403){
                message->statuscode = 400;
        }
	
	message->list = NULL;
}

void processRequest(int client_sockd, struct httpObj* message, threadArg* Arg){
        //Branch the Code Depending on Request Method

        //Process Request for PUT
        if(strcmp(message->method,"PUT") == 0 && message->statuscode == 201){
                //Have to open a file and read in data from file, write to new file
                //open() and use flag O_CREAT | O_TRUNC
		if(strcmp(message->filename,"healthcheck") == 0){message->statuscode = 403;}
		else{
                int fd = open(message->filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                int lock = flock(fd,LOCK_EX);
		struct stat st;
                if(stat(message->filename,&st) ==-1){
                        if(errno == EACCES){
                                message->statuscode = 403;      //Forbidden File
                        }
                        else{
                                message->statuscode = 400;      //Can't open file or create it, so server error
                        }
                }
		
                //char buff[BUFFER_SIZE];         //used to be ssize_t type
                ssize_t rcount = 0;
                ssize_t wcount = 1;
                ssize_t read_bytes = 0;

                //loop through the data and write to file on server
                while(rcount < (message->contentlength) && (message->statuscode == 201)){
                        char* buff = malloc(BUFFER_SIZE*sizeof(char));
			char* node_buffer = malloc(BUFFER_SIZE * sizeof(char));
			memset(buff,0,BUFFER_SIZE);             //Reset the memory in the buffer to avoid garbage
                        read_bytes = read(client_sockd,buff,BUFFER_SIZE);
                        rcount += read_bytes;
			strcpy(node_buffer,BUFFER_SIZE);
                        if(rcount < 0) {
                                message->statuscode = 500;     //Internal error since reading from socket
                                break;
                        }

                        else if(rcount > 0){
                                if(Arg->log_flag == 1 && message->statuscode == 201){ 
                                         Node* new = malloc(sizeof(Node));
                                         new->buffer = node_buffer;
                                         new->next = NULL;
                                         message->list = addLogTwo(message->list,node_buffer,new);
                                         //printLog(message->list);      
                                }
				wcount = write(fd,buff,read_bytes);
				if(wcount < 0){
                                        message->statuscode = 500;      //Internal error: couldn't write to file
                                        break;
                        	}
                        }
                }
		flock(fd,LOCK_UN);
                close(fd);
		}
        }
	//Process Request for HEAD
        else if(strcmp(message->method,"HEAD") == 0 && message->statuscode == 200){
                //FOR HEAD WE SIMPLY RETURN SIZE OF FILE
                //MUST HANDLE open/read errors (400,403)
                if(strcmp(message->filename,"healthcheck") == 0){message->statuscode = 403;}
		else{
		struct stat st;
                int fd = open(message->filename,O_RDONLY);
		int lock = flock(fd,LOCK_EX);
		flock(fd,LOCK_UN);
                if(stat(message->filename,&st) ==-1){
                        if(errno == EACCES){    //This is EACCES CODE
                                message->statuscode = 403;      //Forbidden File
                        }
                        else if(errno == ENOENT){
                                message->statuscode = 404;      //File Not Found
                        }
                        else{
				printf("bad request: file : %s\n",message->filename);
                                message->statuscode = 400;      //Bad request type: couldn't open from file name
                        }
                }
                //We find the size of the file and assign it to message object
                fstat(fd,&st);
                message->contentlength = st.st_size;
                close(fd);
		}
        }
        //Process Request for GET
        else if(strcmp(message->method,"GET") == 0 && message->statuscode == 200){
                //Making the code for GET & Doing the Getting
                if(strcmp(message->filename,"healthcheck") == 0){
			//process healthcheck
			if(Arg->log_flag == 1){
				writeHealthCheck(Arg->log_entries,Arg->error_count,message,Arg,client_sockd);
			}
			else{
				message->statuscode = 404;
			}
		}else{
		struct stat st;
                int fd = open(message->filename,O_RDONLY);
                int lock = flock(fd,LOCK_EX);
		flock(fd,LOCK_UN);
		if(stat(message->filename,&st) == -1){
                        if(errno == EACCES){
                                message->statuscode = 403;      // Forbidden File
                        }
                        else if(errno == ENOENT){
                                message->statuscode = 404;      //File not Found
                        }
                        else{
				printf("bad request- file - %s\n",message->filename);
                                message->statuscode = 400;      //Bad request type couldn't open file
                        }
                }
                //Find size of the file
                fstat(fd,&st);
                message->contentlength = st.st_size;
                close(fd);
		}
        }
}
int countThreads(Node* head){
	int a = 0;
	Node *temp = head;
	while(temp!=NULL){
		a++;
		temp = temp->next;
	}
	return a;
}
void writeLog(Node *head,char *log_file,int offset){ 
         int fd = open(log_file,O_RDWR | O_CREAT,0644);
         if(fd == -1){perror("error opening log\n");}
         //Opened File
	 //printLog(head);
         Node *temp = head;
         int l_offset = offset;
 
         //Write Header
         pwrite(fd,temp->buffer,strlen(temp->buffer),l_offset);
         l_offset += strlen(temp->buffer);
	 temp = temp->next;
 
         //-------------------------------------------------------------------//
         int byteCount = 0;      //Holds the number of chars you've written
         char *line = malloc(9*sizeof(char));
         int lines;
         float a;
         char *data = malloc(22*sizeof(char));
         char* pregame = malloc(9*sizeof(char));
         char hex[3];
         char *whitespace = " ";
         //-------------------------------------------------------------------//
         //loop though linked list
	 int linecount = 0;
	 int flag = 0;
	 int tempCount = countThreads(head);
	 int counter = 0;
         while(temp != NULL && counter < tempCount-1){
		 a = strlen(temp->buffer) / 20.0;
		 lines = ceil(a);
                 //loop through lines
		 //printf("buffering: %s",temp->buffer);
                 for(int i = 0; i < lines; i++){
                 	  
			 sprintf(pregame,"%08d",linecount);
                         pwrite(fd,pregame,8,l_offset);
                         l_offset += 8;
                         strncpy(data,temp->buffer + byteCount,20);
			 
			 if(strlen(data) != 20){
				temp = temp->next;
				if(temp != NULL){
					char* holder = malloc(20 * sizeof(char));
					strncpy(holder,temp->buffer,20-strlen(data));
					strcat(data,holder);
					free(holder);
					flag = 20 - strlen(data);
				}
			 }
                         //printf("doing: ");
			 //Loop through data  to change to hex
                         for(int j = 0; j < strlen(data); j++){ 
                                 
				 unsigned char c = data[j];
				 
				 //printf("%c ",c);
				 snprintf(hex,3,"%02x",c);
				  
				 int chek = pwrite(fd," ",1,l_offset);
                                 if(chek == -1){perror("");}
                                 l_offset += 1;
                                 chek = pwrite(fd,hex,3,l_offset);
                                 if(chek == -1){perror("");}
                                 l_offset += 2;
                         }
			 //printf("\n");
                         byteCount += 20;
			 linecount += 20;
                         pwrite(fd,"\n",1,l_offset);
                         l_offset += 1;
                 }
		 if(flag != 0){byteCount = flag; flag = 0;temp=temp->next;}else{
			byteCount = 0;
			flag = 0;
		 }
		 counter++;
         }
	 free(data);
         free(pregame);
         free(line);
         pwrite(fd,"========\n",9,l_offset);
         close(fd);
}
void makeResponse(int client_sockd,struct httpObj* message,threadArg* Arg){
        char* response = (char*)calloc(BUFFER_SIZE,sizeof(char));
        memset(response,0,BUFFER_SIZE);
        //1. HTTP version
        strcpy(response,message->httpversion);
        //2. STATUS CODE & MESSAGE
	int responselength = 0;
        switch(message->statuscode){
                case 200:
			responselength = message->contentlength;
                        response = strcat(response," 200 OK\r\n");
                        break;
                case 201:
                        response = strcat(response," 201 CREATED\r\n");
			responselength = 0;
                        break;
                case 400:
                        response = strcat(response," 400 BAD REQUEST\r\n");
                        responselength = 0;
                        message->contentlength = 0;
			break;
                case 403:
                        response = strcat(response," 403 FORBIDDEN\r\n");
                        responselength = 0;
			message->contentlength = 0;
                        break;
                case 404:
                        response = strcat(response," 404 FILE NOT FOUND\r\n");
                        responselength = 0;
			message->contentlength = 0;
                        break;
                case 500:
                        response = strcat(response," 500 INTERNAL SERVER ERROR\r\n");
                        responselength = 0;
			message->contentlength = 0;
                        break;
        }
	//3. Content Length
        char length[12];
        sprintf(length,"%ld",responselength);
        response = strcat(response,"Content-Length: ");
        response = strcat(response,length);
        response = strcat(response,"\r\n\r\n");
        
	char* header = (char *)calloc((BUFFER_SIZE), sizeof(char));
	//--------------------------------------------------------------------------------//


	//Write Response to Socket
        size_t len = strlen(response);
        int wbytes = write(client_sockd,response,len);
        
        free(response);
        response = NULL;
	
	//--------------------------------------------------------------------------------//
	
	if(strcmp(message->method,"GET") == 0 && message->statuscode == 200){
		int fd = open(message->filename,O_RDONLY);
        	if(fd < 0){
                	message->statuscode = 500;
        	}
        	char *buff;
        	ssize_t readCount = 0;
        	ssize_t writeCount = 0;
       		ssize_t read_bytes = 0;
        	//Read in Data from File and Write to Socket
        	while(readCount < message->contentlength && message->statuscode == 200){
			buff = malloc(BUFFER_SIZE*sizeof(char));
			char* node_buffer = malloc(BUFFER_SIZE*sizeof(char));
			memset(buff,0,BUFFER_SIZE);
                	read_bytes = read(fd,buff,BUFFER_SIZE);
                	readCount += read_bytes;
			strcpy(node_buffer,buff);
                	if(readCount < 0){
                        	message->statuscode = 500;      //Server Error
                	}
                	else if(readCount > 0){
				if(Arg->log_flag == 1 && message->statuscode == 200){
					
					Node* new = malloc(sizeof(Node));
					new->buffer = node_buffer;
					new->next = NULL;
					//printf("adding buff: %s\n",buff);
					//printf("\n list now: %s\n",new->buffer);
					message->list = addLogTwo(message->list,node_buffer,new);
					//printLog(message->list);
				}
				writeCount = write(client_sockd,buff,read_bytes);
                        	if(writeCount < 0){
                                	message->statuscode = 500;      //Cant write so Internal Server Error
                        	}
                	}
        	}
        	close(fd);
	}
	
	//--------------------------------------------------------------------------------//
	//--------------------------------FIGURE OUT OFFSET-------------------------------//
        if(Arg->log_flag == 1 && (strcmp(message->filename,"healthcheck") != 0)){
		//printf("------TRYING TO SET OFFSET------\n");
                int content_length = message->contentlength;
                char *temp = malloc(100 *sizeof(char));
                sprintf(temp,"%d",content_length);
                
		int method_length = strlen(message->method);
                int filename_length = strlen(message->filename);
                float holder = content_length / 20.0;
                int lines = ceil(holder);

                int header_length = method_length + (filename_length + 1) + 6 + strlen(temp);
                int data_length = (lines * 8) + (content_length * 3) + 1;
                int last_line = 9;
                int offset = header_length + data_length + last_line + 5;
                //int local_offset = Arg->log_offset;
 
                if(message->statuscode != 200 && message->statuscode != 201){
                        sem_wait(&Arg->health_sem);
			Arg->error_count += 1;
			sem_post(&Arg->health_sem);
			sprintf(header,"%s","FAIL: ");
                        offset += 6;
			sprintf(header +strlen(header),"%s /%s %s --- response %d\n",message->method,message->filename,message->httpversion,message->statuscode);
			header_length = strlen(header);
			offset = header_length + last_line;
                }
		else{
                	sprintf(header + strlen(header),"%s /%s length %d \n",message->method,message->filename,content_length);
		}

                sem_wait(&Arg->log_sem);
                int local_offset = Arg->log_offset;
                Arg->log_offset += offset + 1;
		
		//printf("SETTING GLOBAL OFFSET TO: %d\n",offset+1);
                sem_post(&Arg->log_sem);
		
		//Pushing List to the Queue
		message->list = addHeader(message->list,header);
		message->list->local_offset = local_offset;

		//---------------------
		sem_wait(&Arg->log_sem);
		Arg->log_queue = addList(Arg->log_queue,message->list);
		//printf("hii\n");
		//printLog(Arg->log_queue.queue[Arg->log_queue.pos]);
		sem_post(&Arg->writer_sem);
		sem_post(&Arg->log_sem);
		//free(header);
		free(temp);

     	}	
}

//-------------------------------------------------------------------------------------------//
void* connectorThread(void* data){	
	
	int pos = 0;
	threadArg *Arg = (threadArg*)data;
	int *queue = Arg->queue;
	int socket;
	while(1){
		if(sem_wait(&Arg->request_sem) == -1){
			perror("Request wait error:");
		}
			
		if(queue[pos] != NULL){
			socket = queue[pos];
			pos = (pos+1) % BUFFER_SIZE;
			
			//Pass the Socket off to the Server Thread that is free
			sem_wait(&Arg->thread_sem);	//If 0 then no threads left
			
			//printList(Arg->server_queue);

			int id = Arg->server_queue->thread_id;
			Arg->server_queue = removeThread(Arg->server_queue);
			Arg->socket_list[id] = socket;
			sem_post(&Arg->server_sems[id]);	
		}
	}
}

void* writerThread(void* data){
		
	threadArg *Arg = (threadArg*)data;
	
	int fd = open(Arg->log_file,O_RDWR | O_CREAT | O_TRUNC,0644);
	if(fd == -1){perror("");}
	close(fd);
	
	while(1){
		sem_wait(&Arg->writer_sem);
		if(fd == -1){perror("");}
		
		int local_offset = Arg->log_queue.queue[Arg->log_queue.pos]->local_offset;
		//printLog(Arg->log_queue.queue[Arg->log_queue.pos]);
		writeLog(Arg->log_queue.queue[Arg->log_queue.pos],Arg->log_file,local_offset);
		Arg->log_queue = removeList(Arg->log_queue,Arg->log_queue.queue[Arg->log_queue.pos]);
		Arg->log_queue.pos += 1;
		sem_wait(&Arg->health_sem);
		Arg->log_entries += 1;
		sem_post(&Arg->health_sem);
	}
}

void* serverThread(void* data){
	threadArg* Arg = (threadArg*)data;
	int id = Arg->ID++;
	
	while(1){
		
		sem_wait(&Arg->server_sems[id]);	//Wait for a socket to be assigned to the thread
		
		int client_sockd = Arg->socket_list[id];
		//----------This is where we call server functions----------//

		
		struct httpObj message;
		//Read HTTP Message
                readHttpResponse(client_sockd, &message);

                //Process Request
                processRequest(client_sockd, &message,Arg);	

                //Construct HTTP Response
                makeResponse(client_sockd, &message,Arg);
		//----------------------------------------------------------//
		
		Arg->server_queue = insertThread(Arg->server_queue,id);
		close(socket);
		sem_post(&Arg->thread_sem);
	}
}

int main(int argc, char* argv[]){
    	//1. Parse Argv[] In Order to Get Port and Thread Count
	char* port;
	char* logfile;
	int threadCount = 4;
	int opt;
	int logflag = 0;
	if(argc < 2){
		perror("Error: No Port Specified\n");
	}	
	else if(argc == 2){
		port = argv[1];
	}
	else if(argc == 4 || argc == 6){
		
		while( (opt = getopt(argc,argv,"N:l:")) != -1 ){
			switch(opt){
				case 'N':
					threadCount = atoi(optarg);
					break;
				case 'l':
					logflag = 1;
					logfile = optarg; 
					int logcheck = open(logfile, O_CREAT | O_TRUNC,0644);
					close(logcheck);
					break;
			}
		}
		for(; optind < argc; optind++){ //when some extra arguments are passed
      			if(strcmp(argv[optind],"&") != 0){
				port = argv[optind];
			}
   		}
	}
	else{
		perror("Error: Invalid combination of inputs: ");
		fprintf(stderr,"%d\n",argc);
	}
	
	//2. Setup the Server Address and Socket
	if(atoi(port) < 1205){
                perror("Erorr: Port Number invald");
        }	
        struct sockaddr_in server_addr;

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(atoi(port));
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        socklen_t addrlen = sizeof(server_addr);

        //Create server socket
        int server_sockd = socket(AF_INET, SOCK_STREAM, 0);

        // Need to check if server_sockd < 0, meaning an error
        if (server_sockd < 0) {
                perror("socket");
        }

        //Configure server socket
        int enable = 1;

        //This allows you to avoid: 'Bind: Address Already in Use' error
        int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        if(ret < 0){
                perror("Error socket() not bound\n");
                return 1;
        }
 	//Bind server address to socket that is open
        ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);
        if(ret < 0){
                perror("Error address not bound to socket");
                return 1;
        }
        //Listen for incoming connections
        ret = listen(server_sockd, SOMAXCONN); // 5 should be enough, if not use SOMAXCONN

        if (ret < 0) {
                perror("Error in listen()\n");
                return 1;
        }
//-----------------------Server Setup is complete--------------------------//

//-------------------------------------------------------------------------//
	
        //3.Setup Writer and Connector Threads
	threadArg requestQueue;
	requestQueue.log_file = logfile;
	requestQueue.log_flag = logflag;
	requestQueue.log_offset = 0;
	requestQueue.ID = 0;
	requestQueue.log_entries = 0;
	requestQueue.error_count = 0;
	pthread_t writerThreadStruct;
        pthread_t connectorThreadStruct;
	int writerThread_id;
	int connectorThread_id;
	requestQueue.socket_list = (int*)calloc(threadCount,sizeof(int));
	requestQueue.queue = (int*)calloc(BUFFER_SIZE,sizeof(int));	//Allocates memory for the Queue
	memset(requestQueue.queue,-1,BUFFER_SIZE);	
	requestQueue.server_sems = (sem_t*)calloc(threadCount,sizeof(sem_t));
	//---------------------------------------------------------------------------------
	for(int j = 0; j < threadCount; j++){
		sem_init(&requestQueue.server_sems[j],0,0);
	}
	sem_init(&requestQueue.writer_sem,0,0);
	sem_init(&requestQueue.log_sem,0,1);
	sem_init(&(requestQueue.request_sem),0,0);			//Initiates the Semaphore for the Request Queue	
	sem_init(&(requestQueue.thread_sem),0,threadCount);		//Initiates the Semaphore for Threads
	sem_init(&(requestQueue.health_sem),0,1);
	requestQueue.server_queue = makeServerQueue(requestQueue.server_queue,threadCount);
	//---------------------------------------------------------------------------------//
		
	connectorThread_id = pthread_create(&connectorThreadStruct,NULL,connectorThread,(void*)&requestQueue);
	writerThread_id = pthread_create(&writerThreadStruct,NULL,writerThread,(void*)&requestQueue);
	//--------------------------Done With 2 Helper Threads-----------------------------//
        
        //4. For Loop to setup Server Threads
	
	pthread_t server_threads[threadCount];
	int threadCheck;
        for(int i = 0; i < threadCount; i++){
		threadCheck =  pthread_create(&server_threads[i],NULL,serverThread,(void*)&requestQueue);
		if(threadCheck < 0){ perror("Failed to create server threads\n");}
	}	
	/*      
        //Create sockaddr_in with server information
	*/	
	struct sockaddr client_addr;
        socklen_t client_addrlen;
	int pos = 0;

	//---------------TESTING-----------------//
	//---------------------------------------//
	while(1){	

                printf("[+] server is waiting...\n");
		int client_sockd = accept(server_sockd, &client_addr, &client_addrlen); //this returns a socket that we can read and write to

		//printf("WE GOT A REQUEST!\n");
                if(client_sockd < 0){
                        perror("Error: accept()");
                        return 1;
                }
		sem_post(&requestQueue.request_sem);
		requestQueue.queue[pos] = client_sockd;
		pos = (pos+1) % BUFFER_SIZE;
	}
	printf("Closed server\n");
	return 0;
}	
