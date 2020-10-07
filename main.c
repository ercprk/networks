// Date   : September 16, 2020
// Author : Eric Park

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

typedef struct CacheBlock
{
    char *key;
    char *value;
    time_t production;
    time_t expiration;
    struct CacheBlock *moreRU, *lessRU; // For recent usage doubly linked list
    struct CacheBlock *hmPrev, *hmNext; // For hashmap chaining
} CacheBlock;

typedef struct Cache
{
    CacheBlock *mru;
    CacheBlock *lru;
    CacheBlock **hashMap; // each hashMap[index] points to the head of chaining
    unsigned numBlocks;
} Cache;

unsigned getPortNumber(int argc, char **argv);
Cache *createCache();
void deleteCache(Cache *cache);
void serveClient(int sockfd, Cache *cache);
void handleRequest(char *request, char *response, Cache *cache);
void queryServer(char *host, char *request, char *response);
void putIntoCache(Cache *cache, char *key, char *response);
void getFromCache(Cache *cache, char *key, char *response);
void printCache(Cache *cache);
unsigned hashKey(char *key);

#define MAX_URL_LENGTH 100
#define MAX_CONTENT_SIZE 1000000 // 10MB
#define CACHE_SIZE 10
#define HASH_SIZE 13
#define BACKLOG_SIZE 10
#define MAX_SERVING 5
#define CONNECTION_FAIL "Failed to connect to the host\n"

int
main(int argc, char **argv)
{
    int sockfd;
    unsigned portNum;
    struct sockaddr_in addr;
    Cache *cache;

    // Handle input and get port number
    portNum = getPortNumber(argc, argv);

    // Create a TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        fprintf(stderr, "[httpproxy] Failed to create socket in main()\n");
        exit(EXIT_FAILURE);
    }

    // Bind socket to the port number
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(portNum);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    {
        fprintf(stderr, "[httpproxy] Failed to bind socket to port %d\n",
                portNum);
        exit(EXIT_FAILURE);
    }

    // Create cache
    cache = createCache();

    // Serve client
    for (int i = 0; i < MAX_SERVING; i++) serveClient(sockfd, cache);

    // Close socket
    close(sockfd);

    // Delete cache
    deleteCache(cache);

    return 0;
}

// Function  : getPortNumber
// Arguments : int of argc and char ** of argv 
// Does      : 1) checks for the singular argument, port number
//             2) checks the validity of the port number
//             3) returns the port number
// Returns   : unsigned int of port number
unsigned
getPortNumber(int argc, char **argv)
{
    char *rest;
    long portNum;

    // Checks for the singular argument
    if (argc != 2)
    {
        fprintf(stderr, "[httpproxy] Usage: %s <port number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Gets port number and checks validity
    portNum = strtol(argv[1], &rest, 10);
    if (portNum < 1 || portNum > 32767)
    {
        fprintf(stderr, "[httpproxy] Port number, %s, is not valid\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    return (unsigned)portNum;
}

// Function  : createCache
// Arguments : nothing
// Does      : 1) initializes and allocates a cache for the proxy
//             2) returns the pointer to the cache
// Returns   : Cache * of cache
Cache *
createCache()
{
    Cache *cache;
    int i;

    cache = (Cache *)malloc(sizeof(Cache));
    cache->lru = NULL;
    cache->mru = NULL;
    cache->hashMap = (CacheBlock **)malloc(HASH_SIZE * sizeof(CacheBlock *));
    cache->numBlocks = 0;

    for (i = 0; i < HASH_SIZE; i++) cache->hashMap[i] = NULL;

    return cache;
}

// Function  : deleteCache
// Arguments : Cache * of cache
// Does      : 1) deallocates memory in cache
// Returns   : nothing
void
deleteCache(Cache *cache)
{
    CacheBlock *curr, *prev;

    curr = cache->mru;
    while (curr)
    {
        prev = curr;
        curr = curr->lessRU;
        free(prev->key);
        free(prev->value);
        free(prev);
    }
    
    free(cache->hashMap);
    free(cache);
}

// Function  : serveClient
// Arguments : int of socket file descriptor, and Cache * of cache
// Does      : 1) listens on the socket
//             2) accepts the first connection request
//             3) reads an HTTP request
//             4) retrieve the corresponding HTTP response either from the cache
//                or directly from the server
//             5) writes back the HTTP request
// Returns   : nothing
void
serveClient(int sockfd, Cache *cache)
{
    int client_sockfd;
    char *request;
    char *response;
    struct sockaddr *client_addr;
    ssize_t request_size;
    socklen_t *client_addrlen;

    client_addr = malloc(sizeof(struct sockaddr));
    client_addrlen = malloc(sizeof(socklen_t));
    request = malloc(sizeof(char) * MAX_CONTENT_SIZE);
    response = malloc(sizeof(char) * MAX_CONTENT_SIZE);

    // Listen
    if (listen(sockfd, BACKLOG_SIZE) != 0)
    {
        fprintf(stderr, "[httpproxy] Failed listening to port\n");
        fprintf(stderr, "[httpproxy] errno: %d\n", errno);
        exit(EXIT_FAILURE);
    }
    printf("[httpproxy] Listening...\n");

    // Accept the first connection request
    *client_addrlen = sizeof(struct sockaddr_in);
    client_sockfd = accept(sockfd, client_addr, client_addrlen);
    if (client_sockfd < 0)
    {
        fprintf(stderr, "[httpproxy] Failed accepting connection request\n");
        exit(EXIT_FAILURE);
    }
    printf("[httpproxy] Accepted connection request\n");

    // Read from the connection
    bzero(request, sizeof(request));
    request_size = read(client_sockfd, request, MAX_CONTENT_SIZE);
    if (request_size < 0)
    {
        fprintf(stderr, "[httpproxy] Failed reading from the connection\n");
        exit(EXIT_FAILURE);
    }
    request[request_size] = 0; // null-termination for strtok_r
    printf("[httpproxy] Read from the connection\n");

    // Handle request
    bzero(response, MAX_CONTENT_SIZE);
    handleRequest(request, response, cache);

    // Write response to the connection
    if (write(client_sockfd, response, MAX_CONTENT_SIZE) < 0)
    {
        fprintf(stderr, "[httpproxy] Failed writing to the connection\n");
        exit(EXIT_FAILURE);
    }
    printf("[httpproxy] Wrote response to the connection\n");

    // Close client socket
    close(client_sockfd);
    printf("[httpproxy] Closed connection\n");

    free(request);
    free(response);
    free(client_addr);
    free(client_addrlen);
}

// Function  : handleRequest
// Arguments : char * of request, char * of response, and Cache * of cache
// Does      : 1) parses the request
//             2) handles the request appropriately and fills up response
// Returns   : nothing
void
handleRequest(char *request, char *response, Cache *cache)
{
    char *line, *key, *host;
    char *str;
    char *line_saveptr, *get_saveptr, *host_saveptr;
    char line_delim[3] = "\r\n";
    char token_delim[2] = " ";

    printf("[httpproxy] Handling HTTP request\n");

    // Find key
    str = strdup(request); // strtok_r manipulates the string - make a copy
    for (line = strtok_r(str, line_delim, &line_saveptr); line;
         line = strtok_r(NULL, line_delim, &line_saveptr))
    {
        if (strstr(line, "GET ") == line)
        {
            strtok_r(line, token_delim, &get_saveptr);
            key = strtok_r(NULL, token_delim, &get_saveptr);
        }
        if (strstr(line, "Host: ") == line)
        {
            strtok_r(line, token_delim, &host_saveptr);
            host = strtok_r(NULL, token_delim, &host_saveptr);
        }
    }

    // Query cache
    getFromCache(cache, key, response);

    if (response[0] == 0) // If the key-value pair was not in the cache,
    {
        // Directly query server
        queryServer(host, request, response);
        putIntoCache(cache, key, response);
    }

    printCache(cache);

    free(str); // for malloc() within strdup()
}

// Function  : queryServer
// Arguments : char * of request and char * of <hostname>:<portnumber>
// Does      : 1) queries the server of the hostname with the request
//             2) fills up/returns the response from the server
// Returns   : nothing
void
queryServer(char *host, char *request, char *response)
{
    int sockfd;
    long portNum;
    char *saveptr, *rest, *addendum;
    char *hostname;
    struct hostent *server;
    struct sockaddr_in server_addr;
    char delim[2] = ":";

    // Get hostname and port number
    hostname = strtok_r(host, delim, &saveptr);
    addendum = strtok_r(NULL, delim, &saveptr);
    if (addendum)
        portNum = strtol(addendum, &rest, 10);
    else 
        portNum = 80;

    // Create TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        fprintf(stderr, "[httpproxy] Failed to create socket in queryServer\n");
        exit(EXIT_FAILURE);
    }

    // Get server information
    server = gethostbyname(hostname);
    if (server == NULL)
    {
        fprintf(stderr, "[httpproxy] No such host as %s\n", hostname);
        fprintf(stderr, "[httpproxy] h_errno: %d\n", h_errno);
        strcpy(response, "No such host indicated by the hostname\n");
        return;
    }

    // Build the server's Internet address
    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,(char *)&server_addr.sin_addr.s_addr,
           server->h_length);
    server_addr.sin_port = htons(portNum);

    // Connect with the server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr))
        < 0) 
    {
        fprintf(stderr, "[httpproxy] Failed to connect to %s on port %ld\n",
                hostname, portNum);
        strcpy(response, CONNECTION_FAIL);
        return;
    }

    // Send the HTTP request to the server
    if(write(sockfd, request, strlen(request)) < 0)
        fprintf(stderr, "[httpproxy] Failed to write to %s\n", hostname);
    printf("[httpproxy] Querying host %s\n", hostname);
    
    // Read the HTTP response from the server
    if (read(sockfd, response, MAX_CONTENT_SIZE) < 0)
        fprintf(stderr, "[httpproxy] Failed to read from %s\n");
    printf("[httpproxy] Received response from host %s\n", hostname);

    close(sockfd);
}

// Function  : putIntoCache
// Arguments : Cache * of cache, char * of key, and char * of response
// Does      : 1) Hashes the key
//             2) Puts the key-response pair in an appropriate place in cache
// Returns   : nothing
void
putIntoCache(Cache *cache, char *key, char *response)
{
    CacheBlock *newBlock, *currBlock;
    char *line_saveptr, *cache_saveptr, *rest;
    char *str, *line, *token;
    unsigned hash;
    long maxAge;
    char line_delim[3] = "\r\n";
    char cache_delim[9] = "max-age=";

    hash = hashKey(key);
    printf("[httpproxy] Caching key %s into cache\n", key);

    // Find max age
    str = strdup(response); // strtok_r manipulates the string
    for (line = strtok_r(str, line_delim, &line_saveptr); line;
         line = strtok_r(NULL, line_delim, &line_saveptr))
    {
        if (strstr(line, "Cache-Control: ") == line)
        {
            strtok_r(line, cache_delim, &cache_saveptr);
            token = strtok_r(NULL, cache_delim, &cache_saveptr);

            if (token)
                maxAge = strtol(token, &rest, 10);
            else
                maxAge = 3600;
        }
    }

    newBlock = malloc(sizeof(CacheBlock));
    newBlock->key = strdup(key);
    newBlock->value = strdup(response);
    
    // Recent usage linked list operations
    newBlock->moreRU = NULL; // New block is always the MRU
    newBlock->lessRU = cache->mru;
    if (!cache->lru) cache->lru = newBlock;
    if (cache->mru) cache->mru->moreRU = newBlock;
    cache->mru = newBlock;

    // Hash map operations
    currBlock = cache->hashMap[hash];
    if (currBlock) // If there's something already in hash at position hash
    {
        while (currBlock->hmNext) currBlock = currBlock->hmNext;
        currBlock->hmNext = newBlock;
        newBlock->hmPrev = newBlock;
    }
    else // If there's nothing at hashed key yet
    {
        cache->hashMap[hash] = newBlock;
        newBlock->hmPrev = NULL;
    }
    newBlock->hmNext = NULL; // New block always at the end of a hash chaining

    cache->numBlocks++;

    free(str);
}

// Function  : getFromCache
// Arguments : Cache * of cache, char * of key, and char * of response
// Does      : 1) Searches the cache for the key
//             2) returns/fills in the response with the corresponding response
// Returns   : response is a null string if the result is not found
void
getFromCache(Cache *cache, char *key, char *response)
{
    CacheBlock *curr;
    unsigned hash;

    hash = hashKey(key);
    bzero(response, sizeof(response));

    curr = cache->hashMap[hash];
    while (curr)
    {
        if (strcmp(key, curr->key) == 0)
        {
            strcpy(response, curr->value);
            return;
        }

        curr = curr->hmNext;
    }
}

// Function  : organizeCache
// Arguments : Cache * of cache
// Does      : 1) This function is called when space needs to be cleared up
//             2) removes any stale cache block
//             3) if it's still full, remove the LRU block
// Returns   : nothing
void
organizeCache(Cache *cache)
{
    CacheBlock *curr;

    curr = cache->mru;
    while (curr)
    {
        
    }
}

// Function  : printCache
// Arguments : Cache * of cache, char * of key, and char * of response
// Does      : 1) Searches the cache for the key
//             2) returns/fills in the response with the corresponding response
// Returns   : nothing
void
printCache(Cache *cache)
{
    CacheBlock *curr, *prev;
    unsigned counter = 0;

    curr = cache->mru;
    while (curr)
    {
        prev = curr;
        curr = curr->lessRU;
        printf("[httpproxy] [cache] Recency: %d / KEY: %s / AGE: %d\n", counter,
               prev->key, 0);
        counter++;
    }
}

// Function  : hashKey
// Arguments : char * of a string key
// Does      : 1) hashes the string key into an integer index
// Returns   : unsigned of hashed value
// Reference : The C Programming Language (Kernighan & Ritchie), Section 6.6
unsigned
hashKey(char *key)
{
    unsigned hashval;

    for (hashval = 0; *key != '\0'; key++)
        hashval = *key + 31 * hashval;

    return hashval % HASH_SIZE;
}