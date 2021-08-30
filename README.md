**# Assignment 3 - Load Balancer**

The load balancer distributes incoming connections to a set of prespecified 
servers. The load balancer frequently checks the status of the servers and 
prioritizes the server with the least number of requests performed thus far. 
The load balancer does not parse through the client’s message and instead 
forwards it to the server and only uses the number of requests to prioritize 
which server to use.


 Objectives 
1. Parse command line arguments for client port, server port, threads, and request frequency
2. Implement multithreading to ensure that multiple incoming connections can be handled 
3. Implement health checks on servers to select the server with least number of requests and ensure synchronization
4. Bridge connections between client and server

## Building the program
| Command | Description |
| ---      |  ------  |
| make | makes loadbalancer |
| make all | makes loadbalancer |
| make clean | Removes loadbalancer |
|make spotless | 	Removes loadbalancer and .o files |



### Running the program

Runs loadbalancer 

`./loadbalancer Client_PORTNUMBER SERVER_PORTNUMBER-R request_frequency_to_probe_healthcheck -N num_of_worker_Threads`


-R and -N are optional arguements. At least 1 client and server Port number must be specifed. Multiple Servers can be specified