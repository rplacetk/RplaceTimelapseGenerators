#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <png.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "console.h"
#include "canvas-downloader.h"
#include "image-generator.h"
#include "main-thread.h"
#include "canvas-saver.h"
#include "worker-structs.h"
#include <stdbool.h>

// Reads canvas list, manages jobs for workers
#define LOG_HEADER "[main thread] "

// SHARED BETWEEN-WORKER MEMORY
#define STACK_SIZE_MAX 256
#define DEFAULT_DOWNLOAD_WORKER_COUNT 2
struct canvas_info download_stack[STACK_SIZE_MAX];
int download_stack_top = -1;
#define DEFAULT_RENDER_WORKER_COUNT 4
struct downloaded_result render_stack[STACK_SIZE_MAX];
int render_stack_top = -1;
#define DEFAULT_SAVE_WORKER_COUNT 2
struct render_result save_stack[STACK_SIZE_MAX];
int save_stack_top = -1;

// WORKER THREADS
#define WORKER_MAX 100
struct worker_info* download_workers[WORKER_MAX] = { };
int download_worker_count = 0;
struct worker_info* render_workers[WORKER_MAX] = { };
int render_worker_count = 0;
struct worker_info* save_workers[WORKER_MAX] = { };
int save_worker_count = 0;

int width = 1000;
int height = 1000;
int backups_finished = 0;
char backups_dir[256];

pthread_mutex_t work_wait_mutex;
pthread_mutex_t download_pop_mutex;
bool download_stack_replenished = false;
pthread_mutex_t render_pop_mutex;
bool render_stack_replenished = false;
pthread_mutex_t save_pop_mutex;
bool save_stack_replenished = false;

// Private
void init_work_queue(struct main_thread_queue* queue, size_t capacity)
{
    queue->work = (struct main_thread_work*) malloc(sizeof(struct main_thread_work) * capacity);
    if (!queue->work)
    {
        stop_console();
        fprintf(stderr, "Failed to initialise main thread work queue\n");
        exit(EXIT_FAILURE);
    }

    queue->capacity = capacity;
    queue->front = 0;
    queue->rear = 0;
    pthread_mutex_init(&queue->mutex, NULL);
}

// Dequeue a message
void push_work_queue(struct main_thread_queue* queue, struct main_thread_work work)
{
    pthread_mutex_lock(&queue->mutex);

    // Check for queue overflow
    if ((queue->rear + 1) % queue->capacity == queue->front)
    {
        stop_console();
        fprintf(stderr, "Error - Work queue overflow.\n");
        pthread_mutex_unlock(&queue->mutex);
        exit(EXIT_FAILURE);
    }

    // Enqueue the message
    queue->work[queue->rear] = work;
    queue->rear = (queue->rear + 1) % queue->capacity;

    pthread_mutex_unlock(&queue->mutex);
    pthread_mutex_unlock(&work_wait_mutex); 
}

struct main_thread_work pop_work_queue(struct main_thread_queue* queue)
{
    pthread_mutex_lock(&queue->mutex);

    // Check underflow
    if (queue->front == queue->rear)
    {
        stop_console();
        fprintf(stderr, "Error - Work queue underflow.\n");
        pthread_mutex_unlock(&queue->mutex);
        exit(EXIT_FAILURE);
    }

    // Dequeue the message
    struct main_thread_work message = queue->work[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    pthread_mutex_unlock(&queue->mutex);

    return message;
}

// Fetch methods
// - https://raw.githubusercontent.com/rslashplace2/rslashplace2.github.io/82153c44c6a3cd3f248c3cd20f1ce6b7f0ce4b1e/place
// - https://github.com/rslashplace2/rslashplace2.github.io/blob/82153c44c6a3cd3f248c3cd20f1ce6b7f0ce4b1e/place?raw=true
// - https://github.com/rslashplace2/rslashplace2.github.io/raw/82153c44c6a3cd3f248c3cd20f1ce6b7f0ce4b1e/place
int flines(FILE* file)
{
    fseek(file, 0, SEEK_SET);
    char ch = 0;
    int count = 0;
    while ((ch = getc(file)) != EOF)
    {
        if (ch == '\n')
        {
            count++;
        }
    }
    fseek(file, 0, SEEK_SET);
    return count;
}

void add_download_worker()
{
    pthread_t thread_id;
    struct worker_info* info = (struct worker_info*) malloc(sizeof(struct worker_info));
    info->worker_id = download_worker_count;
    pthread_create(&thread_id, NULL, start_download_worker, info);
    info->thread_id = thread_id;
    download_workers[download_worker_count] = info;
    download_worker_count++;
}

void add_render_worker()
{
    pthread_t thread_id;
    struct worker_info* info = (struct worker_info*) malloc(sizeof(struct worker_info));
    info->worker_id = render_worker_count;
    pthread_create(&thread_id, NULL, start_render_worker, info);
    info->thread_id = thread_id;
    render_workers[render_worker_count] = info;
    render_worker_count++;
}

void add_save_worker()
{
    pthread_t thread_id;
    struct worker_info* info = (struct worker_info*) malloc(sizeof(struct worker_info));
    info->worker_id = save_worker_count;
    pthread_create(&thread_id, NULL, start_save_worker, info);
    info->thread_id = thread_id;
    save_workers[save_worker_count] = info;
    save_worker_count++;
}

// Post queue
#define MAIN_THREAD_QUEUE_SIZE 64
struct main_thread_queue work_queue;

// Public
// Enques work to work quueue - 
void main_thread_post(struct main_thread_work work)
{
    push_work_queue(&work_queue, work);
}

// Called by main thread
void push_download_stack(struct canvas_info result)
{
    download_stack[++download_stack_top] = result;
    download_stack_replenished = true;

    if (download_stack_top >= STACK_SIZE_MAX)
    {
        stop_console();
        fprintf(stderr, "Error - download_stack overflow occurred\n");
        exit(EXIT_FAILURE);
    }
}

// Called by download worker
void push_render_stack(struct downloaded_result result)
{
    render_stack[++render_stack_top] = result;
    render_stack_replenished = true;

    if (render_stack_top > STACK_SIZE_MAX - 1)
    {
        stop_console();
        fprintf(stderr, "Error - render_stack overflow occurred\n");
        exit(EXIT_FAILURE);
    }
}

// Called by render worker
void push_save_stack(struct render_result result)
{
    save_stack[++save_stack_top] = result;
    save_stack_replenished = true;

    if (save_stack_top > STACK_SIZE_MAX - 1)
    {
        stop_console();
        fprintf(stderr, "Error - push_stack overflow occurred\n");
        exit(EXIT_FAILURE);
    }
}

// Forward declarations
void* read_commit_hashes(FILE* file);
FILE* commit_hashes_stream = NULL;

// Called by download worker
struct canvas_info pop_download_stack(int worker_id)
{
    pthread_mutex_lock(&download_pop_mutex);

    if (download_stack_top < STACK_SIZE_MAX / 2)
    {
        // Replenish, we assume download will take long enough, and read will be fast to not cause issues
        main_thread_post((struct main_thread_work) { .func = (void (*)(void *))read_commit_hashes, .data=commit_hashes_stream });
    }
    if (download_stack_top == 0)
    {
        log_message(LOG_HEADER"Download worker %d waiting on canvas_info from download stack...", worker_id);
        while (!download_stack_replenished)
        {
            usleep(1000);
        }
        download_stack_replenished = false;
    }
    else if (download_stack_top < 0)
    {
        stop_console();
        fprintf(stderr, "Error - download_stack underflow occurred\n");
        exit(EXIT_FAILURE);
    }

    struct canvas_info result = download_stack[download_stack_top];
    download_stack_top--;
    
    pthread_mutex_unlock(&download_pop_mutex);
    return result;
}

// Called by render worker
struct downloaded_result pop_render_stack(int worker_id)
{
    pthread_mutex_lock(&render_pop_mutex);

    // Try and just wait for something to please come in
    if (render_stack_top < 0)
    {
        log_message(LOG_HEADER"Render worker %d waiting on download_result from render stack...", worker_id);
        while (!render_stack_replenished)
        {
            usleep(1000);
        }
        render_stack_replenished = false;
    }

    struct downloaded_result result = render_stack[render_stack_top];
    render_stack_top--;
    
    pthread_mutex_unlock(&render_pop_mutex);
    return result;
}

// Called by save worker
struct render_result pop_save_stack(int worker_id)
{
    pthread_mutex_lock(&save_pop_mutex);

    // Try and just wait for something to please come in
    if (save_stack_top < 0)
    {
        log_message(LOG_HEADER"Save worker %d waiting on render_result from save stack...", worker_id);
        while (!save_stack_replenished)
        {
            usleep(1000);
        }
        save_stack_replenished = false;
    }

    struct render_result result = save_stack[save_stack_top];
    save_stack_top--;

    pthread_mutex_unlock(&save_pop_mutex);
    return result;
}

#define MAX_HASHES_LINE_LEN 256
#define EXPECT_COMMIT_LINE 0
#define EXPECT_AUTHOR_LINE 1
#define EXPECT_DATE_LINE 2

// Commit: ...\n, Author: ...\n, Date: ...\n, top is most recent, bottom of log is longest ago
//  - Both files have been manually assimilated into commit_hashes.txt
//  - canvas_1_commit_hashes.txt - only process commits from "root", from after 1672531200,
//  - commit (1672531012, 48a24088916a63319a56c245a0290d66cf29e076) is the first commit (Jan 1st), 2023
//  - repo_commit_hashes.txt - Only process commits from "nebulus"
/*Commit: 49fdda2c4f38f7afd8af2c178b450e8f0fcae65b
Author: nebulus
Date: 1704298864*/
// called by main thread
void* read_commit_hashes(FILE* file)
{
    int expect = EXPECT_COMMIT_LINE;
    long file_lines = flines(file);
    struct canvas_info new_canvas_info = {};
    char line[MAX_HASHES_LINE_LEN];
    char* result = NULL;
    int line_index = 0;
    while ((result = fgets(line, MAX_HASHES_LINE_LEN, file)) != NULL)
    {
        line_index++;
        // Comment or ignore
        if (strlen(result) == 0 || result[0] == '#' || result[0] == '\n')
        {
            continue;
        }

        if (strncmp(result, "Commit: ", 8) == 0)
        {
            if (expect != EXPECT_COMMIT_LINE)
            {
                log_message("(Line %d:%s) expected Commit property, skipping", line_index, line);
                continue;
            }
            
            int result_len = strlen(result);
            int hash_len = result_len - 9;
            char* commit_hash = malloc(hash_len + 1);
            result[result_len - 1] = '\0';
            strcpy(commit_hash, result + 8);
            new_canvas_info.commit_hash = commit_hash;
            const char* raw_url = "https://raw.githubusercontent.com/rslashplace2/rslashplace2.github.io/%s/place";
            int url_length = snprintf(NULL, 0, raw_url, commit_hash);
            char* url = (char*) malloc(url_length + 1);
            snprintf(url, url_length + 1, raw_url, commit_hash); // + 1?
            new_canvas_info.url = url;
            expect = EXPECT_AUTHOR_LINE;
        }
        else if (strncmp(result, "Author: ", 8) == 0)
        {
            if (expect != EXPECT_AUTHOR_LINE)
            {
                log_message("(Line %d:%s) expected Author property, skipping", line_index, line);
                continue;
            }

            char* author = result + 8;
            result[strlen(result) - 1] = '\0';
            if (strcmp(author, "root") != 0 && strcmp(author, "nebulus") != 0)
            {
                // Ignore this commit, it is not a canvas push
                expect = EXPECT_COMMIT_LINE;
                memset(&new_canvas_info, 0, sizeof(struct canvas_info)); // Wipe for reuse
                continue;
            }
            expect = EXPECT_DATE_LINE;
        }
        else if (strncmp(result, "Date: ", 6) == 0)
        {
            if (expect != EXPECT_DATE_LINE)
            {
                log_message("(Line %d:%s) expected Date property, skipping", line_index, line);
                continue;
            }

            int date_len = strlen(result) - 7;
            char* date = malloc(date_len + 1);
            strcpy(date, result + 7);
            time_t date_int = strtoull(date, NULL, 10);
            new_canvas_info.date = date_int;
            free(date);

            push_download_stack(new_canvas_info);
            memset(&new_canvas_info, 0, sizeof(struct canvas_info)); // Wipe for reuse

            // Commit data to download stack
            if (download_stack_top >= STACK_SIZE_MAX - 1)
            {
                // We will come back later
                log_message(LOG_HEADER"Bufferred %d commit records into download stack. Pausing until needs replenish", download_stack_top + 1);
                break;
            }
            expect = EXPECT_COMMIT_LINE;
        }
        else
        {
            stop_console();
            fprintf(stderr, "(Line %d:%s) Failed to read commit hashes, invalid character\n", line_index, line);
            exit(EXIT_FAILURE);
        }
    }
    return NULL;
}

// Start all workers, initiate rendering backups 
void start_generation()
{
    // Setup backups dir variable
    getcwd(backups_dir, sizeof(backups_dir));
    strcat(backups_dir, "/backups/");
    mkdir(backups_dir, 0755);

    FILE* file = fopen("commit_hashes.txt", "r");
    if (file == NULL)
    {
        stop_console();
        fprintf(stderr, "\x1b[1;31mError, could not locate canvas 1 hashes file (commit_hashes.txt)\n");
        exit(EXIT_FAILURE);
    }
    commit_hashes_stream = file;

    long file_lines = flines(file);
    log_message(LOG_HEADER"Detected %d lines in commit_hashes.txt", file_lines);
    read_commit_hashes(file);

    // fclose
    log_message(LOG_HEADER"Starting backup generation");
    for (int i = 0; i < DEFAULT_DOWNLOAD_WORKER_COUNT; i++)
    {
        add_download_worker();
    }
    for (int i = 0; i < DEFAULT_SAVE_WORKER_COUNT; i++)
    {
        add_render_worker();
    }
    for (int i = 0; i < DEFAULT_SAVE_WORKER_COUNT; i++)
    {
        add_save_worker();
    }
}

// Often called by UUI. Cleanly shutdown generation side of program, will cleanup all resources
void stop_generation()
{
    // Cleanup work queue
    free(work_queue.work);
    pthread_mutex_destroy(&work_queue.mutex);

    // TODO: Terminate and cleanup all workers
    // foreach download worker -> curl_easy_cleanup(curl_handle), etc
    
    // Cleanuup globals
    curl_global_cleanup();
    pthread_exit(NULL);
    exit(0);
}

void safe_segfault_exit(int sig_num)
{
    sleep(1);
    stop_console();
    fprintf(stderr, "FATAL - Segmantation fault\n");
    exit(EXIT_FAILURE);
}

void start_main_thread()
{
    signal(SIGSEGV, safe_segfault_exit);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    init_work_queue(&work_queue, MAIN_THREAD_QUEUE_SIZE);
    pthread_mutex_init(&work_wait_mutex, NULL);
    pthread_mutex_init(&download_pop_mutex, NULL);
    pthread_mutex_init(&render_pop_mutex, NULL);
    pthread_mutex_init(&save_pop_mutex, NULL);

    pthread_mutex_lock(&work_wait_mutex);
    while (1)
    {
        // Will wait for work to arrive via work queue, at which point
        // it will unlock and main thread will pop & process work
        pthread_mutex_lock(&work_wait_mutex);
        struct main_thread_work work = pop_work_queue(&work_queue);
        work.func(work.data);
    }
    
}