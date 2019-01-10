//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000 // 取值范围等距切分,切分粒度;将[-6,6)切分成EEXP_TABLE_SIZE份
#define MAX_EXP 6 //sigmoid 自变量取值范围
#define MAX_SENTENCE_LENGTH 1000 // 单句最大长度;用于对语料库中句子进行切分,如果句子长度太长的话;
#define MAX_CODE_LENGTH 40 //huffman编码最大长度

/*
 * The size of the hash table for the vocabulary.
 * The vocabulary won't be allowed to grow beyond 70% of this number.
 * For instance, if the hash table has 30M entries, then the maximum
 * vocab size is 21M. This is to minimize the occurrence (and performance
 * impact) of hash collisions.
 */
const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary

//重命名 float实数
typedef float real;                    // Precision of float numbers

/**
 * ======== vocab_word ========
 * Properties:
 *   cn - The word frequency (number of times it appears).
 *   word - The actual string word.
 */
// 词的结构体,保存在词典中
struct vocab_word {
  long long cn;//出现次数
  int *point;//从根节点到叶子节点的路径
  char *word, *code, codelen;//分别对应着词,Huffman编码[l^w-1位编码;l^w表示路径中包含的结点个数],编码长度
};

/*
 * ======== Global Variables ========
 *
 */
char train_file[MAX_STRING], output_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];

/*
 * ======== vocab ========
 * This array will hold all of the words in the vocabulary.
 * This is internal state.
 */
struct vocab_word *vocab;

int binary = 0, cbow = 1, debug_mode = 2, window = 5, min_count = 5, num_threads = 12, min_reduce = 1;

/*
 * ======== vocab_hash ========
 * This array is the hash table for the vocabulary. Word strings are hashed
 * to a hash code (an integer), then the hash code is used as the index into
 * 'vocab_hash', to retrieve the index of the word within the 'vocab' array.
 */
// 存储字典的hash值,键为word的hash code,值为word在词典中的下标index;
int *vocab_hash;

/*
 * ======== vocab_max_size ========
 * This is not a limit on the number of words in the vocabulary, but rather
 * a chunk size for allocating the vocabulary table. The vocabulary table will
 * be expanded as necessary, and is allocated, e.g., 1,000 words at a time.
 *
 * ======== vocab_size ========
 * Stores the number of unique words in the vocabulary. 
 * This is not a parameter, but rather internal state. 
 *
 * ======== layer1_size ========
 * This is the number of features in the word vectors.
 * It is the number of neurons in the hidden layer of the model.
 */
long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100;

/*
 *
 */
long long train_words = 0, word_count_actual = 0, iter = 5, file_size = 0, classes = 0;

/*
 * ======== alpha ========
 * TODO - This is a learning rate parameter.
 *
 * ======== starting_alpha ========
 *
 * ======== sample ========
 * This parameter controls the subsampling of frequent words.
 * Smaller values of 'sample' mean words are less likely to be kept.
 * Set 'sample' to 0 to disable subsampling.
 * See the comments in the subsampling section for more details.
 */
real alpha = 0.025, starting_alpha, sample = 1e-3;

real *syn0, *syn1, *syn1neg, *expTable;
clock_t start;

int hs = 0, negative = 5;
const int table_size = 1e8;
int *table;

/**
 * ======== InitUnigramTable ========
 * 计算negative sampling 抽样转换表
 * M = table_size >> N = vocab_size;
 * 使用:
 * 输入一个随机数n,范围在[0,vocab_size)以内,可以得到抽样结果:抽样词在词典中的下标
 * 
 * table[random_num] = word_index
 * 
 * 思想:[0,1]区间,根据词典中词的归一化词频依据每个词在vocab中下标,依次将[0,1]区间进行划分,
 * 左边界是区间的标识,比如说word_index(0) = fraction(0);词频大的词对应小区间长度就长;
 * 这样vocab_size个小区间,每个区间对应一个词(标识,vocab中下标);
 * 
 * 然后,我们再将这个[0,1]重新进行等距划分,也划分成M个小区间,这样,如果将两种划分结果,映射到
 * 同一个[0,1]区间上,那么第二种方法划分标识可能会落在第一种标识范围内.这样,就可以实现带权抽样.
 * 
 * 输入一个第二种划分方法的标识,可以得到一个第一种划分的标识[词].
 */
void InitUnigramTable() {
  int a, i;
  double train_words_pow = 0;
  double d1, power = 0.75;
  //抽样表
  table = (int *)malloc(table_size * sizeof(int));
  for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
  i = 0;
  // 当前词的归一化词频,小区间长度
  d1 = pow(vocab[i].cn, power) / train_words_pow;
  for (a = 0; a < table_size; a++) {//将[0,1]划分为table_size个小区间,左边界作为区间标识
    table[a] = i;//可能存在多个随机数抽样结果都是i
    // [Chris] - If the fraction of the table we have filled is greater than the
    //           fraction of this words weight / all word weights, then move to the next word.
    // 等距划分每个子区间长度为:1/table_size;
    // 随机数对应第二种方法得到的子区间长度映射到第一种方法,得到映射后结果
    if (a / (double)table_size > d1) {//如果等距划分大于当前词的区间长度,划分到下一个词
      i++;
      d1 += pow(vocab[i].cn, power) / train_words_pow;//累加上计算下一个词的区间长度;得到下一个词对应区间的右边界
      // 大于这个边界,映射后就是下一个词
    }
    // 划分结束
    if (i >= vocab_size) i = vocab_size - 1;
  }
}

/**
 * ======== ReadWord ========
 * Reads a single word from a file, assuming space + tab + EOL to be word 
 * boundaries.
 *
 * Parameters:
 *   word - A char array allocated to hold the maximum length string.
 *   fin  - The training file.
 */
void ReadWord(char *word, FILE *fin) {
  
  // 'a' will be the index into 'word'.
  int a = 0, ch;
  
  // Read until the end of the word or the end of the file.
  while (!feof(fin)) {
  
    // Get the next character.
    ch = fgetc(fin);
    
    // ASCII Character 13 is a carriage return 'CR' whereas character 10 is 
    // newline or line feed 'LF'.
    if (ch == 13) continue;
    
    // Check for word boundaries...
    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      // If the word has at least one character, we're done.
      if (a > 0) {
        // Put the newline back before returning so that we find it next time.
        if (ch == '\n') ungetc(ch, fin);
        break;
      }
      // If the word is empty and the character is newline, treat this as the
      // end of a "sentence" and mark it with the token </s>.
      if (ch == '\n') {
        strcpy(word, (char *)"</s>");
        return;
      // If the word is empty and the character is tab or space, just continue
      // on to the next character.     
      } else continue;
    }
    
    // If the character wasn't space, tab, CR, or newline, add it to the word.
    word[a] = ch;
    a++;
    
    // If the word's too long, truncate it, but keep going till we find the end
    // of it.
    if (a >= MAX_STRING - 1) a--;   
  }
  
  // Terminate the string with null.
  word[a] = 0;
}

/**
 * ======== GetWordHash ========
 * Returns hash value of a word. The hash is an integer between 0 and 
 * vocab_hash_size (default is 30E6).
 *
 * For example, the word 'hat':
 * hash = ((((h * 257) + a) * 257) + t) % 30E6
 */
int GetWordHash(char *word) {
  unsigned long long a, hash = 0;
  for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
  hash = hash % vocab_hash_size;
  return hash;
}

/**
 * ======== SearchVocab ========
 * Lookup the index in the 'vocab' table of the given 'word'.
 * Returns -1 if the word is not found.
 * This function uses a hash table for fast lookup.
 */
int SearchVocab(char *word) {
  // Compute the hash value for 'word'.
  unsigned int hash = GetWordHash(word);
  
  // Lookup the index in the hash table, handling collisions as needed.
  // See 'AddWordToVocab' to see how collisions are handled.
  while (1) {
    // If the word isn't in the hash table, it's not in the vocab.
    if (vocab_hash[hash] == -1) return -1;
    
    // If the input word matches the word stored at the index, we're good,
    // return the index.
    if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
    
    // Otherwise, we need to scan through the hash table until we find it.
    hash = (hash + 1) % vocab_hash_size;
  }
  
  // This will never be reached.
  return -1;
}

/**
 * ======== ReadWordIndex ========
 * Reads the next word from the training file, and returns its index into the
 * 'vocab' table.
 */
int ReadWordIndex(FILE *fin) {
  char word[MAX_STRING];
  ReadWord(word, fin);
  if (feof(fin)) return -1;
  return SearchVocab(word);
}

/**
 * ======== AddWordToVocab ========
 * Adds a new word to the vocabulary (one that hasn't been seen yet).
 */
int AddWordToVocab(char *word) {
  // Measure word length.
  unsigned int hash, length = strlen(word) + 1;
  
  // Limit string length (default limit is 100 characters).
  if (length > MAX_STRING) length = MAX_STRING;
  
  // Allocate and store the word string.
  vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
  strcpy(vocab[vocab_size].word, word);
  
  // Initialize the word frequency to 0.
  vocab[vocab_size].cn = 0;
  
  // Increment the vocabulary size.
  vocab_size++;
  
  // Reallocate memory if needed
  if (vocab_size + 2 >= vocab_max_size) {
    vocab_max_size += 1000;
    vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
  }
  
  // Add the word to the 'vocab_hash' table so that we can map quickly from the
  // string to its vocab_word structure. 
  
  // Hash the word to an integer between 0 and 30E6.
  hash = GetWordHash(word);
  
  // If the spot is already taken in the hash table, find the next empty spot.
  while (vocab_hash[hash] != -1) 
    hash = (hash + 1) % vocab_hash_size;
  
  // Map the hash code to the index of the word in the 'vocab' array.  
  vocab_hash[hash] = vocab_size - 1;
  
  // Return the index of the word in the 'vocab' array.
  return vocab_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
    return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

/**
 * ======== SortVocab ========
 * Sorts the vocabulary by frequency using word counts, and removes words that
 * occur fewer than 'min_count' times in the training text.
 * 
 * Removing words from the vocabulary requires recomputing the hash table.
 */
void SortVocab() {
  int a, size;
  unsigned int hash;
  
  /*
   * Sort the vocabulary by number of occurrences, in descending order. 
   *
   * Keep </s> at the first position by sorting starting from index 1.
   *
   * Sorting the vocabulary this way causes the words with the fewest 
   * occurrences to be at the end of the vocabulary table. This will allow us
   * to free the memory associated with the words that get filtered out.
   */
  qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
  
  // Clear the vocabulary hash table.
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  
  // Store the initial vocab size to use in the for loop condition.
  size = vocab_size;
  
  // Recompute the number of training words.
  train_words = 0;
  
  // For every word currently in the vocab...
  for (a = 0; a < size; a++) {
    // If it occurs fewer than 'min_count' times, remove it from the vocabulary.
    if ((vocab[a].cn < min_count) && (a != 0)) {
      // Decrease the size of the new vocabulary.
      vocab_size--;
      
      // Free the memory associated with the word string.
      free(vocab[a].word);
    } else {
      // Hash will be re-computed, as after the sorting it is not actual
      hash=GetWordHash(vocab[a].word);
      while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
      vocab_hash[hash] = a;
      train_words += vocab[a].cn;
    }
  }
   
  // Reallocate the vocab array, chopping off all of the low-frequency words at
  // the end of the table.
  vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
  
  // Allocate memory for the binary tree construction
  for (a = 0; a < vocab_size; a++) {
    vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
    vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
  }
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
  int a, b = 0;
  unsigned int hash;
  for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {
    vocab[b].cn = vocab[a].cn;
    vocab[b].word = vocab[a].word;
    b++;
  } else free(vocab[a].word);
  vocab_size = b;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  for (a = 0; a < vocab_size; a++) {
    // Hash will be re-computed, as it is not actual
    hash = GetWordHash(vocab[a].word);
    while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
    vocab_hash[hash] = a;
  }
  fflush(stdout);
  min_reduce++;
}

/**
 * ======== CreateBinaryTree ========
 * Create binary Huffman tree using the word counts.
 * Frequent words will have short unique binary codes.
 * Huffman encoding is used for lossless compression.
 * The vocab_word structure contains a field for the 'code' for the word.
 */
void CreateBinaryTree() {
  long long a, b, i, min1i, min2i, pos1, pos2, point[MAX_CODE_LENGTH];
  char code[MAX_CODE_LENGTH];
  long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));//词典词count数组,统计词频
  long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  //词频数组count初始化:容量为vocab_size *2 + 1
  // 词典中词初始化,正常初始化
  for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;
  // 超出词典大小的词频,设置为1e15(一个极大数)
  for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;
  pos1 = vocab_size - 1;
  pos2 = vocab_size;
  // Following algorithm constructs the Huffman tree by adding one node at a time
  for (a = 0; a < vocab_size - 1; a++) {
    // First, find two smallest nodes 'min1, min2'
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min1i = pos1;
        pos1--;
      } else {
        min1i = pos2;
        pos2++;
      }
    } else {
      min1i = pos2;
      pos2++;
    }
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min2i = pos1;
        pos1--;
      } else {
        min2i = pos2;
        pos2++;
      }
    } else {
      min2i = pos2;
      pos2++;
    }
    count[vocab_size + a] = count[min1i] + count[min2i];
    parent_node[min1i] = vocab_size + a;
    parent_node[min2i] = vocab_size + a;
    binary[min2i] = 1;
  }
  // Now assign binary code to each vocabulary word
  for (a = 0; a < vocab_size; a++) {
    b = a;
    i = 0;
    while (1) {
      code[i] = binary[b];
      point[i] = b;
      i++;
      b = parent_node[b];
      if (b == vocab_size * 2 - 2) break;
    }
    vocab[a].codelen = i;
    vocab[a].point[0] = vocab_size - 2;
    for (b = 0; b < i; b++) {
      vocab[a].code[i - b - 1] = code[b];
      vocab[a].point[i - b] = point[b] - vocab_size;
    }
  }
  free(count);
  free(binary);
  free(parent_node);
}

/**
 * ======== LearnVocabFromTrainFile ========
 * Builds a vocabulary from the words found in the training file.
 *
 * This function will also build a hash table which allows for fast lookup
 * from the word string to the corresponding vocab_word object.
 *
 * Words that occur fewer than 'min_count' times will be filtered out of
 * vocabulary.
 */
void LearnVocabFromTrainFile() {
  char word[MAX_STRING];
  FILE *fin;
  long long a, i;
  
  // Populate the vocab table with -1s.
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  
  // Open the training file.
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  
  vocab_size = 0;
  
  // The special token </s> is used to mark the end of a sentence. In training,
  // the context window does not go beyond the ends of a sentence.
  // 
  // Add </s> explicitly here so that it occurs at position 0 in the vocab. 
  AddWordToVocab((char *)"</s>");
  
  while (1) {
    // Read the next word from the file into the string 'word'.
    ReadWord(word, fin);
    
    // Stop when we've reached the end of the file.
    if (feof(fin)) break;
    
    // Count the total number of tokens in the training text.
    train_words++;
    
    // Print progress at every 100,000 words.
    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
      printf("%lldK%c", train_words / 1000, 13);
      fflush(stdout);
    }
    
    // Look up this word in the vocab to see if we've already added it.
    i = SearchVocab(word);
    
    // If it's not in the vocab...
    if (i == -1) {
      // ...add it.
      a = AddWordToVocab(word);
      
      // Initialize the word frequency to 1.
      vocab[a].cn = 1;
    
    // If it's already in the vocab, just increment the word count.
    } else vocab[i].cn++;
    
    // If the vocabulary has grown too large, trim out the most infrequent 
    // words. The vocabulary is considered "too large" when it's filled more
    // than 70% of the hash table (this is to try and keep hash collisions
    // down).
    if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
  }
  
  // Sort the vocabulary in descending order by number of word occurrences.
  // Remove (and free the associated memory) for all the words that occur
  // fewer than 'min_count' times.
  SortVocab();
  
  // Report the final vocabulary size, and the total number of words 
  // (excluding those filtered from the vocabulary) in the training set.
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  
  file_size = ftell(fin);
  fclose(fin);
}

void SaveVocab() {
  long long i;
  FILE *fo = fopen(save_vocab_file, "wb");
  for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
  fclose(fo);
}

void ReadVocab() {
  long long a, i = 0;
  char c;
  char word[MAX_STRING];
  FILE *fin = fopen(read_vocab_file, "rb");
  if (fin == NULL) {
    printf("Vocabulary file not found\n");
    exit(1);
  }
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  vocab_size = 0;
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    a = AddWordToVocab(word);
    fscanf(fin, "%lld%c", &vocab[a].cn, &c);
    i++;
  }
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  fseek(fin, 0, SEEK_END);
  file_size = ftell(fin);
  fclose(fin);
}

/**
 * ======== InitNet ========
 *
 */
void InitNet() {
  long long a, b;
  unsigned long long next_random = 1;
  
  // Allocate the hidden layer of the network, which is what becomes the word vectors.
  // The variable for this layer is 'syn0'.
  // 为隐藏层分配空间,syn0;word vectors;长数组,并不是矩阵形式;所以每次取之前,都要计算词向量在长数组中的index;
  a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
  
  if (syn0 == NULL) {printf("Memory allocation failed\n"); exit(1);}
  
  // If we're using hierarchical softmax for training...
  if (hs) {//如果使用hierarchical softmax,对应的,会生成huffman tree,进而需要theta参数,节点参数theta
    //theta向量大小和layer1_size大小相同,其实并没有这么大,非叶子结点个数为n-1个(n是叶子节点个数,大小等于vocab_size)
    a = posix_memalign((void **)&syn1, 128, (long long)vocab_size * layer1_size * sizeof(real));
    if (syn1 == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
     syn1[a * layer1_size + b] = 0;
  }
  
  // If we're using negative sampling for training...
  if (negative>0) {
    // Allocate the output layer of the network. 
    // The variable for this layer is 'syn1neg'.
    // This layer has the same size as the hidden layer, but is the transpose.
    // 输出层的数据,大小和hidden层向量相同;存储输出层词向量数组
    a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
    
    if (syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
    
    // Set all of the weights in the output layer to 0.
    for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
     syn1neg[a * layer1_size + b] = 0;
  }
  
  // Randomly initialize the weights for the hidden layer (word vector layer).
  // TODO - What's the equation here?
  // 隐藏层word vector layer 权重初始化
  for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++) {
    next_random = next_random * (unsigned long long)25214903917 + 11;
    syn0[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / layer1_size;
  }
  
  // Create a binary tree for Huffman coding.
  // TODO - As best I can tell, this is only used for hierarchical softmax training...
  CreateBinaryTree();
}

/**
 * ======== TrainModelThread ========
 * This function performs the training of the model.
 * 模型训练子例程,方便多线程调用
 */
void *TrainModelThread(void *id) {

  /*
   * word - Stores the index of a word in the vocab table.
   * word_count - Stores the total number of training words processed.
   */
  long long a, b, d, cw, word, last_word, sentence_length = 0, sentence_position = 0;
  long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
  long long l1, l2, c, target, label, local_iter = iter;
  unsigned long long next_random = (long long)id;
  real f, g;
  clock_t now;
  
  // neu1 is only used by the CBOW architecture.
  // neu1仅仅在CBOW模型中使用
  real *neu1 = (real *)calloc(layer1_size, sizeof(real));
  
  // neu1e is used by both architectures.
  // neu1e在两个模型中都用到;输出层对projection layer向量的梯度更新量
  real *neu1e = (real *)calloc(layer1_size, sizeof(real));
  
  
  // Open the training file and seek to the portion of the file that this 
  // thread is responsible for.
  // 处理数据,由于是多线程,需要对输入文件根据线程数目划分出每个线程负责的数量,用于线程训练;
  FILE *fi = fopen(train_file, "rb");
  fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
  
  // This loop covers the whole training operation...
  while (1) {
    // This block prints a progress update, and also adjusts the training 
    // 'alpha' parameter.
    if (word_count - last_word_count > 10000) {
      word_count_actual += word_count - last_word_count;
      last_word_count = word_count;
      if ((debug_mode > 1)) {
        now=clock();
        printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
         word_count_actual / (real)(iter * train_words + 1) * 100,
         word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
        fflush(stdout);
      }
      // 修改学习率;动态修改,随着训练过程地进行,学习率逐渐降低
      alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));
      if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
    }
    
    // This 'if' block retrieves the next sentence from the training text and
    // stores it in 'sen'.
    // TODO - Under what condition would sentence_length not be zero?
    // 从训练数据中,读取下一条句子,句子长度为MAX_SENTENCE_LENGTH
    if (sentence_length == 0) {//是否需要读取一个新句子get a new sentence,保存到sen数组中[sen数组保存处理的当前句]
      while (1) {
        // Read the next word from the training data and lookup its index in 
        // the vocab table. 'word' is the word's vocab index.
        word = ReadWordIndex(fi);
        
        if (feof(fi)) break;
        
        // If the word doesn't exist in the vocabulary, skip it.
        if (word == -1) continue;
        
        // Track the total number of training words processed.
        word_count++;
        
        // 'vocab' word 0 is a special token "</s>" which indicates the end of 
        // a sentence.
        if (word == 0) break;//句子终止符
        
        /* 
         * =================================
         *   Subsampling of Frequent Words
         * =================================
         * This code randomly discards training words, but is designed to 
         * keep the relative frequencies the same. That is, less frequent
         * words will be discarded less often. 
         *
         * We first calculate the probability that we want to *keep* the word;
         * this is the value 'ran'. Then, to decide whether to keep the word,
         * we generate a random fraction (0.0 - 1.0), and if 'ran' is smaller
         * than this number, we discard the word. This means that the smaller 
         * 'ran' is, the more likely it is that we'll discard this word. 
         *
         * The quantity (vocab[word].cn / train_words) is the fraction of all 
         * the training words which are 'word'. Let's represent this fraction
         * by x.
         *
         * Using the default 'sample' value of 0.001, the equation for ran is:
         *   ran = (sqrt(x / 0.001) + 1) * (0.001 / x)
         * 
         * You can plot this function to see it's behavior; it has a curved 
         * L shape.
         * 
         * Here are some interesting points in this function (again this is
         * using the default sample value of 0.001).
         *   - ran = 1 (100% chance of being kept) when x <= 0.0026.
         *      - That is, any word which is 0.0026 of the words *or fewer* 
         *        will be kept 100% of the time. Only words which represent 
         *        more than 0.26% of the total words will be subsampled.
         *   - ran = 0.5 (50% chance of being kept) when x = 0.00746. 
         *   - ran = 0.033 (3.3% chance of being kept) when x = 1.
         *       - That is, if a word represented 100% of the training set
         *         (which of course would never happen), it would only be
         *         kept 3.3% of the time.
         *
         * NOTE: Seems like it would be more efficient to pre-calculate this 
         *       probability for each word and store it in the vocab table...
         *
         * Words that are discarded by subsampling aren't added to our training
         * 'sentence'. This means the discarded word is neither used as an 
         * input word or a context word for other inputs.
         */
        // 对高频词进行降采样 subsamping for frequent words
        if (sample > 0) {
          // Calculate the probability of keeping 'word'.计算词word保存的概率
          real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
          
          // Generate a random number.
          // The multiplier is 25.xxx billion, so 'next_random' is a 64-bit integer.
          // 生成一个随机数
          next_random = next_random * (unsigned long long)25214903917 + 11;

          // If the probability is less than a random fraction, discard the word.
          //
          // (next_random & 0xFFFF) extracts just the lower 16 bits of the 
          // random number. Dividing this by 65536 (2^16) gives us a fraction
          // between 0 and 1. So the code is just generating a random fraction.
          // 随机数归一化,然后和保存概率ran比较,判断词word是否应该删除
          if (ran < (next_random & 0xFFFF) / (real)65536) continue;
        }
        
        // If we kept the word, add it to the sentence.
        // 如果保留,添加到句子(数组)中,添加的是这个词word在字典中对应的下标index
        sen[sentence_length] = word;
        sentence_length++;
        
        // Verify the sentence isn't too long.
        // 如果句子长度过长,截断处理
        if (sentence_length >= MAX_SENTENCE_LENGTH) break;
      }
      //句子中指针位置,中心词w位置
      sentence_position = 0;
    }
    // feof(fi)文件结束,返回非0值;反之,返回0
    // 处理语料末尾数据:语料终止,最后数据量不足
    if (feof(fi) || (word_count > train_words / num_threads)) {
      word_count_actual += word_count - last_word_count;
      local_iter--;
      if (local_iter == 0) break;
      word_count = 0;
      last_word_count = 0;
      sentence_length = 0;
      fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
      continue;
    }
    
    // Get the next word in the sentence. The word is represented by its index
    // into the vocab table.
    // 得到当前中心词;在句子中取一个词word,取出的是word在字典中对应的下标index
    word = sen[sentence_position];
    
    if (word == -1) continue;//如果没找到,继续

    // 模型参数初始化
    //cbow模型中,xw向量,输入向量累和 neu1 projection layer向量
    for (c = 0; c < layer1_size; c++) neu1[c] = 0;
    //e,用来对词向量进行更新 output layer every non-leaf node's theta parameters,the dimension of vector theta is same with the neu1
    for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
    
    // b随机初始化, b是中心词两边读取词长度大小
    next_random = next_random * (unsigned long long)25214903917 + 11;
    // 'b' becomes a random integer between 0 and 'window'.
    // This is the amount we will shrink the window size by.
    // 确定b大小,[0,window);
    b = next_random % window;//每个中心词对应上下文取值边界范围,由于是随机数,导致每个中心词上下文取值范围是不同的,window还有什么用,只是指定了一个范围
    
    /* 
     * ====================================
     *        CBOW Architecture
     * ====================================
     */
    if (cbow) {  //train the cbow architecture
      // in -> hidden, sum up all the word vectors of all the words in the window
      // 输入层->隐层;将window里词的词向量累加,得到映射层向量xw
      cw = 0;//cw保存选择词的数目,词向量的个数
      /*
      * 扫描目标单词的左右几个单词
      * ===================================
      * sentence_position:i 处理的当前中心词
      * 上下文边界取值c: [i-(window-b), i+(window-b)];
      * window-b是上下文边界范围;中心词i前面window-b个,后面window-b个;window-b=c,一共2c个上下文向量,一般情况;
      * 如果上下文取值超出句子边界(超过句子头,超出句子尾),上下文向量就不一定了,所以需要cw来统计取值到的上下文向量个数
      * 当a=window时,c=i-window+a=i-window+window=i;等于当前中心词,跳过;
      * 这样就保证neu1是sentence_position中心词的上下文向量累积和,而且不包括当前中心词.
      */
      for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
        c = sentence_position - window + a;
        if (c < 0) continue;
        if (c >= sentence_length) continue;
        last_word = sen[c];
        if (last_word == -1) continue;
        //syn0: 应该是将所有的词向量拼接到一个长向量里了;向量长度为:layer1_size*n_words,所以需要确定是word在常向量里的位置
        // syn0 词典词向量数组; 将读取的上下文累加,得到projection layer向量neu1
        for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size];//syn0[index] index词的词向量
        cw++;//统计读取词向量数目
      }
      if (cw) {
        // 对projection layer累加向量neu1取平均;
        for (c = 0; c < layer1_size; c++) neu1[c] /= cw;//对累加向量取均值
        /* 
        * 开始训练
        * 不同策略的训练过程有所不同
        */
       //=======================================================================
        /*
        * 1.hierarchical softmax sgd更新方法:每个样本(w,context(w))进行一次更新
        * 这种策略依赖于huffman树,每个非叶子结点对应有一个参数,参数维度和词向量维度相同;
        * 而每次分支,也就是说每个非叶子结点都看做是一次二分类过程,分类方法采用logistic regression方法;
        * 而分类结果由每条分支的Huffman编码确定,word2vec认为左分支编码为1,分为负类;右分支编码为0,分为正类;
        * huffman树的构造主要是用来拟合条件概率p(w|context(w)),我们用一个函数来拟合这个条件概率F(w,context(w);\theta) = p(w|context(w)),
        * 避免繁琐的统计过程来计算条件概率,大大节省了空间以及时间.
        * 依据huffman树结构,比如说叶子节点w对应的huffman编码为111,那么条件概率p(w|context(w))计算过程为:
        * p(w|context(w)) = (1-sigma(theta_1*c))*(1-sigma(theta_2*c))*(1-sigma(theta_4*c)) 
        * 所以,我们需要先得到非叶子结点的参数theta,然后才能计算条件概率;
        */
        // 针对当前中心词,计算条件概率p(w|context(w));计算过程依赖于中心词word的huffman code;codelen存储huffman code的code length
        if (hs) for (d = 0; d < vocab[word].codelen; d++) {//word中心词
          //1. 前向传播
          f = 0;//保存sigma(theta*c),分为正类的概率(编码为0)
          //vocab结构体中point存储着叶子节点路径[点集,从根节点到这个叶子节点]
          l2 = vocab[word].point[d] * layer1_size;//得到当前非叶子结点index,然后计算在参数数组中的偏移位置
          // Propagate hidden -> output
          // 一次分类:非叶子结点分类logistic regression
          for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1[c + l2];
          if (f <= -MAX_EXP) continue;
          else if (f >= MAX_EXP) continue;
          else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
          //2. 反向传播 sgd更新
          //L(w,j)对theta偏导数以及L(w,j)对c_w偏导数中的重合量,两个梯度中都有这个量
          //为了方便,我们提前计算,之后重复使用
          g = (1 - vocab[word].code[d] - f) * alpha;
          // Propagate errors output -> hidden;可以看做是output->hidden的反向传播过程
          // 计算当前非叶子结点对上下文向量c_w的更新量;由于c_w参与了路径上的所有非叶子结点的分类过程,
          // 所以对context(w)上下文中的每个向量更新时,都需要先累计所有分类过程的更新量,最后再对上下文中词向量进行更新
          // 累计梯度更新量;对c_w的梯度
          for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
          // 更新当前非叶子结点的参数theta
          for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * neu1[c];
        }
        /* 
        * 2.NEGATIVE SAMPLING方法
        * 网络结构也是三层(如果输入层算一层的话): Input -> Projection -> Output
        * Input layer是上下文向量集context(w);
        * Projection layer是上下文向量和c(w),有可能会取平均;上下文向量context(w)也是通过采样选择的;
        * Output layer是vocab_size大小的词向量集;
        * 
        * Same with hierarchical softmax neural network, 这个网络结构也是用来计算条件概率p(w|context(w)).
        * 这种方法依赖于负采样策略;
        * 
        * 负采样策略:简单来说是对vocab_size大小的词集的带权负采样过程,权重是word在语料中的词频大小,通常会做归一化;权重越大,越有可能被选中,权重小,选中可能性就比较小.
        * 这里将vocab_size个词的归一化权重,累积到[0,1]线段上,不同权的词所占的长度不同,但这个长度正好是它的权重大小-----权重统计;
        * 我们的采样过程,始终要对应到采样的词上,但并不是直接选词,还要考虑到权重;所以,这里存在一个与权重相关的映射关系,考虑到[0,1]线段;
        * 我们将这个线段分为M份(M >> N),然后生成一个大小在M范围内的随机数,然后这个随机数在映射到[0,1]线段上,看它落在哪个区间,这个区间对应权重的词,就是我们的取样词.
        * 
        * 存在一个trick:[0,1]线段的每个词的权重,并不是真正的词频,而是取了一个3/4次幂,为什么是这个数?因为效果好.
        * 
        * 最重要的条件概率p(w|context(w))怎么计算?举例来说,已知中心词w和上下文词向量context(w),以及中心词w的负采样样本neg(w);neg(w)中不包含中心词w本身;
        * 这个计算过程包括多个分类过程,每个分类过程:在上下文context(w)已知的情况下,计算p(u|context(w))出现的概率,其中词u取值范围$u \in \{w\} \cup neg(w)$,
        * 语言描述就是说,u属于中心词w和w的负采样本集neg(w)两个集合的并集内;那么正负类的确定,如果u=w,就属于正类,反之,属于负类,将两个统一后,label表示分类结果,可以得到:
        * p(u|context(w))={sigma(u*c)}^label * {1-sigma(u*c)}^(1-label).
        * 了解了上下文已知的情况下,每个词的分类效果,那么条件概率p(w|context(w))怎么计算呢?
        * 
        * 类似于cbow+hs方法,条件概率是多次分类过程的连乘积;negative sampling方法,这个条件概率是negative+1个分类过程决定;
        * p(w|context(w))=p(w|context(w))* \prod_{u \in neg(w)} p(u|context(w))
        * 
        * 这是一个样本的条件概率
        */
        if (negative > 0) for (d = 0; d < negative + 1; d++) {
          if (d == 0) {//处理当前中心词
            target = word;
            label = 1;
          } else {//neg(w)采样
            next_random = next_random * (unsigned long long)25214903917 + 11;
            target = table[(next_random >> 16) % table_size];//负采样结果,词下标
            if (target == 0) target = next_random % (vocab_size - 1) + 1;
            // 如果采样到当前中心词,跳过,进行下一次采样
            if (target == word) continue;
            label = 0;
          }
          // 获取当前词的词向量
          l2 = target * layer1_size;// 计算偏置
          f = 0;//sigmoid函数值
          // 前向传播
          //neu1存储projection 的上下文词向量和c(w);syn1neg存储词向量数组,输出层结果,
          for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1neg[c + l2];//找到抽样词的词向量
          //计算 关于上下文和c(w)和当前抽样词word u梯度的重合部分g
          if (f > MAX_EXP) g = (label - 1) * alpha;//sigmoid = 1
          else if (f < -MAX_EXP) g = (label - 0) * alpha;//sigmoid = 0
          else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
          // 更新c(w)
          for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2];
          // 更新抽样词u的词向量v(u)
          for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * neu1[c];
        }
        // hidden -> in
        // 对上下文context(w)中词向量更新[组成上下文的每个词向量]
        for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
          c = sentence_position - window + a;
          if (c < 0) continue;
          if (c >= sentence_length) continue;
          last_word = sen[c];
          if (last_word == -1) continue;
          // 更新context(w)中的词向量
          for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += neu1e[c];
        }
      }
    } 
    /* 
     * ====================================
     *        Skip-gram Architecture
     * ====================================
     * 
     * 神经网路的用途在于计算条件概率p(context(w)|w),用神经网络来拟合条件概率函数F(w,context(w),theta) = p(context(w)|w)
     * 
     * sen - This is the array of words in the sentence. Subsampling has already been
     *       applied. I don't know what the word representation is...
     *
     * sentence_position - This is the index of the current input word.
     *
     * a - Offset into the current window, relative to the window start.
     *     a will range from 0 to (window * 2) (TODO - not sure if it's inclusive or
     *      not).
     *
     * c - 'c' is the index of the current context word *within the sentence*
     *
     * syn0 - The hidden layer weights.
     *
     * l1 - Index into the hidden layer (syn0). Index of the start of the
     *      weights for the current input word.
     */
    else {  
      // Loop over the positions in the context window (skipping the word at
      // the center). 'a' is just the offset within the window, it's not 
      // the index relative to the beginning of the sentence.
      // 上下文抽样context(w)
      for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
        
        // Convert the window offset 'a' into an index 'c' into the sentence 
        // array.
        c = sentence_position - window + a;//上下文抽样词u
        
        // Verify c isn't outisde the bounds of the sentence.
        if (c < 0) continue;
        if (c >= sentence_length) continue;
        
        // Get the context word. That is, get the id of the word (its index in
        // the vocab table).
        // 上下文抽样词u,得到index
        last_word = sen[c];
        
        // At this point we have two words identified:
        //   'word' - The word at our current position in the sentence (in the
        //            center of a context window).
        //   'last_word' - The word at a position within the context window.
        
        // Verify that the word exists in the vocab (I don't think this should
        // ever be the case?)
        if (last_word == -1) continue;
        
        // Calculate the index of the start of the weights for 'last_word'.
        // 得到上下文抽样词的词向量,先计算词向量在总词向量数组中的偏置
        l1 = last_word * layer1_size;
        // 累计更新,关于v(w);
        for (c = 0; c < layer1_size; c++) neu1e[c] = 0;

        /*
        * 1. HIERARCHICAL SOFTMAX
        * =======================================
        * 拟合的函数形式F(w,context(w),theta) = p(context(w)|w).
        * 首先,需要知道的一点是由中心词w预测上下文context(w);上下文context(w)并不是一个词,或者说不是一个向量,
        * 包括中心词前c个向量,后c个向量.所以条件概率p(context(w)|w)会和之前的p(w|context(w))有所不同.
        * 假设,中心词w的上下文向量context(w),其中抽样词u \in context(w).
        * 那么,条件概率p(context(w)|w)计算方法:
        * p(context(w)|w) = \prod p(u|w)
        * 
        * 这样就转换成求每个条件向量p(u|w),把v(w)当做输出向量,u当做预测.但是,由于context(w)中包含不同的u.
        * 
        * 为了和之前的更新方式同意起来,代码一致.
        * 
        * Google工程师发现:如果w为中心词时,u在w的上下文窗口内;那么,当u为中心词时,w也在u的上下文窗口内.
        * 那么,cost function可以转换,变成和cbow形式差不多的形式,只不过context(w)变成了一个向量,这里是抽样词,
        * 也就是说cbow的p(w|context(w)),skip-gram原来的p(u|w)变成p(w|u),u是上下文,不过变成了一个向量;w还是中心词.
        * 
        * 这样两种模型loss function几乎相同,统一起来了.
        * 
        * 语料库的loss function为:
        * \sum_{s=1}^S \sum_{t=1}^T \sum_{-c<=j<=c}log(w_{t+j}|w_t) = \sum_{s=1}^S \sum_{t=1}^T \sum_{-c<=j<=c}log(w_t|w_{t+j})
        * 
        * 意思是:计算语料库中所有句子的语言概率---遍历语料库中所有句子,每条句子每个中心词,每个中心词对应的上下文的条件概率.
        * 从右到左,最里的sum是计算上下文概率p(context(w)|w),再外一层是计算句子概率,最外层是计算整个语料库所有句子的语言概率和.
        * 
        * 
        * 转换成和cbow类似的分类过程,也是p(w|context(w)),不过context(w)是由一个向量组成.
        * p(w|u)
        */
        if (hs) for (d = 0; d < vocab[word].codelen; d++) {//计算p(w|u) u是上下文抽样词的一个;进行codelen次分类过程
          f = 0;
          l2 = vocab[word].point[d] * layer1_size;
          // Propagate hidden -> output
          // syn0抽样词,和cbow中的c(w)一样; syn1是每个非叶子结点的参数theta
          // l1上下文抽样词下标;l2非叶子结点分类过程对应参数
          for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1[c + l2];
          if (f <= -MAX_EXP) continue;
          else if (f >= MAX_EXP) continue;
          else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
          // 'g' is the gradient multiplied by the learning rate
          // 计算两个梯度中的重合量,方便重复使用
          g = (1 - vocab[word].code[d] - f) * alpha;
          // Propagate errors output -> hidden
          // 计算关于u累积量,因为u参与了所有word的分类过程,所以要累计,最后在用累计量对u词向量进行一次更新
          for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
          // Learn weights hidden -> output
          // 更新每个分类过程的参数
          for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * syn0[c + l1];
        }
        
        /* 
        * 2. NEGATIVE SAMPLING p(context(w)|w)
        * ===========================================================
        * 和cbow+neg方法类似,并不是对所有词向量更新,只更新负采样的样本集
        * 
        * 同时和skip-gram模型的特点
        * p(context(w)|w) = \prod_{u \in context(w)} p(u|w) = \prod_{u \in context(w)}p(w|u)
        * 
        */
        if (negative > 0) for (d = 0; d < negative + 1; d++) {//负采样过程
          // On the first iteration, we're going to train the positive sample.
          // 使用负采样,抽取一个负样本;d=0时,采样正样本,所以负采样次数为negative + 1次
          if (d == 0) {
            target = word;
            label = 1;
          // On the other iterations, we'll train the negative samples.
          } else {
            // Pick a random word to use as a 'negative sample'; do this using 
            // the unigram table.
            
            // Get a random integer.
            next_random = next_random * (unsigned long long)25214903917 + 11;
            
            // 'target' becomes the index of the word in the vocab to use as
            // the negative sample.
            target = table[(next_random >> 16) % table_size];
            
            // If the target is the special end of sentence token, then just
            // pick a random word from the vocabulary instead.
            if (target == 0) target = next_random % (vocab_size - 1) + 1;
            
            // Don't use the positive sample as a negative sample!
            if (target == word) continue;
            
            // Mark this as a negative example.
            label = 0;
          }
          
          // Get the index of the target word in the output layer.
          l2 = target * layer1_size;
          
          // At this point, our two words are represented by their index into
          // the layer weights.
          // l1 - The index of our input word within the hidden layer weights.
          // l2 - The index of our output word within the output layer weights.
          // label - Whether this is a positive (1) or negative (0) example.
          
          // Calculate the dot-product between the input words weights (in 
          // syn0) and the output word's weights (in syn1neg).
          f = 0;
          //syn0 上下文向量,条件; syn1neg 负采样样本
          for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1neg[c + l2];
          
          // This block does two things:
          //   1. Calculates the output of the network for this training
          //      pair, using the expTable to evaluate the output layer
          //      activation function.
          //   2. Calculate the error at the output, stored in 'g', by
          //      subtracting the network output from the desired output, 
          //      and finally multiply this by the learning rate.
          // 计算两个梯度计算过程的重合量
          if (f > MAX_EXP) g = (label - 1) * alpha;
          else if (f < -MAX_EXP) g = (label - 0) * alpha;
          else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
          
          // Multiply the error by the output layer weights.
          // (I think this is the gradient calculation?)
          // Accumulate these gradients over all of the negative samples.
          // 关于条件u的累计梯度更新量
          for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2];
          
          // Update the output layer weights by multiplying the output error
          // by the hidden layer weights.
          // 负采样抽样样本梯度更新
          for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * syn0[c + l1];
        }
        // Once the hidden layer gradients for all of the negative samples have
        // been accumulated, update the hidden layer weights.
        // 负采样完成后,对条件u对应向量进行一次性更新
        for (c = 0; c < layer1_size; c++) syn0[c + l1] += neu1e[c];
      }
    }
    
    // Advance to the next word in the sentence.
    // 获取下一个训练样本--当前句子,更换w,context(w)
    sentence_position++;
    
    // Check if we've reached the end of the sentence.
    // If so, set sentence_length to 0 and we'll read a new sentence at the
    // beginning of this loop.
    // 如果到当前句子末尾,读取下一个句子
    if (sentence_position >= sentence_length) {
      sentence_length = 0;
      continue;
    }
  }
  fclose(fi);
  free(neu1);
  free(neu1e);
  pthread_exit(NULL);
}

/**
 * ======== TrainModel ========
 * Main entry point to the training process.
 */
void TrainModel() {
  long a, b, c, d;
  FILE *fo;
  
  pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));//多线程;线程数组
  
  printf("Starting training using file %s\n", train_file);
  
  starting_alpha = alpha;//初始学习率;学习率动态变动
  
  // Either load a pre-existing vocabulary, or learn the vocabulary from 
  // the training file.
  // 区分是否指定词库;如果指定,读取词库文件;否则,从训练语料中学习;
  if (read_vocab_file[0] != 0) ReadVocab(); else LearnVocabFromTrainFile();
  
  // Save the vocabulary.判断是否需要保存词库
  if (save_vocab_file[0] != 0) SaveVocab();
  
  // Stop here if no output_file was specified. 如果没有指定保存文件,直接退出;[保存文件是指词向量保存文件]
  if (output_file[0] == 0) return;
  
  // Allocate the weight matrices and initialize them.
  // 网络初始化
  InitNet();

  // If we're using negative sampling, initialize the unigram table, which
  // is used to pick words to use as "negative samples" (with more frequent
  // words being picked more often).
  // 如果使用负采样,初始化unigram table  
  if (negative > 0) InitUnigramTable();
  
  // Record the start time of training.
  // 计时,debug提示信息
  start = clock();
  
  // Run training, which occurs in the 'TrainModelThread' function.
  // 多线程训练,加快训练速度
  for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
  for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
  
  // 输出最终的词向量训练结果
  fo = fopen(output_file, "wb");
  if (classes == 0) {
    // Save the word vectors
    // 保存,将内容写到fo文件中,先写字典长度,词向量长度参数
    fprintf(fo, "%lld %lld\n", vocab_size, layer1_size);
    for (a = 0; a < vocab_size; a++) {
      // 保存格式: word --- word_vector
      // 先写词word
      fprintf(fo, "%s ", vocab[a].word);
      // 保存这个词的词向量;判断是否以二进制形式保存
      if (binary) for (b = 0; b < layer1_size; b++) fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
      else for (b = 0; b < layer1_size; b++) fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
      fprintf(fo, "\n");
    }
  } else {
    // 对词向量进行聚类
    // Run K-means on the word vectors
    int clcn = classes, iter = 10, closeid;
    int *centcn = (int *)malloc(classes * sizeof(int));
    int *cl = (int *)calloc(vocab_size, sizeof(int));
    real closev, x;
    real *cent = (real *)calloc(classes * layer1_size, sizeof(real));
    for (a = 0; a < vocab_size; a++) cl[a] = a % clcn;
    for (a = 0; a < iter; a++) {
      for (b = 0; b < clcn * layer1_size; b++) cent[b] = 0;
      for (b = 0; b < clcn; b++) centcn[b] = 1;
      for (c = 0; c < vocab_size; c++) {
        for (d = 0; d < layer1_size; d++) cent[layer1_size * cl[c] + d] += syn0[c * layer1_size + d];
        centcn[cl[c]]++;
      }
      for (b = 0; b < clcn; b++) {
        closev = 0;
        for (c = 0; c < layer1_size; c++) {
          cent[layer1_size * b + c] /= centcn[b];
          closev += cent[layer1_size * b + c] * cent[layer1_size * b + c];
        }
        closev = sqrt(closev);
        for (c = 0; c < layer1_size; c++) cent[layer1_size * b + c] /= closev;
      }
      for (c = 0; c < vocab_size; c++) {
        closev = -10;
        closeid = 0;
        for (d = 0; d < clcn; d++) {
          x = 0;
          for (b = 0; b < layer1_size; b++) x += cent[layer1_size * d + b] * syn0[c * layer1_size + b];
          if (x > closev) {
            closev = x;
            closeid = d;
          }
        }
        cl[c] = closeid;
      }
    }
    // Save the K-means classes
    for (a = 0; a < vocab_size; a++) fprintf(fo, "%s %d\n", vocab[a].word, cl[a]);
    free(centcn);
    free(cent);
    free(cl);
  }
  fclose(fo);
}
//解析命令行:根据参数名称,在运行参数中查找,如果找到返回,对应下标;之后根据下标对程序中相应参数进行赋值
int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
    if (a == argc - 1) {
      printf("Argument missing for %s\n", str);
      exit(1);
    }
    return a;//找到,返回下标
  }
  return -1;
}

int main(int argc, char **argv) {
  int i;
  //判断参数个数
  if (argc == 1) {
    printf("WORD VECTOR estimation toolkit v 0.1c\n\n");//Word Vector 计算
    printf("Options:\n");//运行选项
    printf("Parameters for training:\n");//训练参数
    printf("\t-train <file>\n");//模型训练的文本数据文件
    printf("\t\tUse text data from <file> to train the model\n");
    printf("\t-output <file>\n");//指定保存词向量/词簇的文件
    printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
    printf("\t-size <int>\n");//词向量维度,默认为100
    printf("\t\tSet size of word vectors; default is 100\n");
    printf("\t-window <int>\n");//窗口大小;n-gram模型里的n参数
    printf("\t\tSet max skip length between words; default is 5\n");
    printf("\t-sample <float>\n");//设置词出现的频率阈值.对高频词会进行降采样;默认为0.001,通常取值在(0,1e-5)内
    printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency in the training data\n");
    printf("\t\twill be randomly down-sampled; default is 1e-3, useful range is (0, 1e-5)\n");
    printf("\t-hs <int>\n");//是否使用分层softmax,hierarchical softmax;默认是0,不使用;
    printf("\t\tUse Hierarchical Softmax; default is 0 (not used)\n");
    printf("\t-negative <int>\n");//negative sampling采用负取样时负样本采样数目,默认是5,有效范围是[3,10],0表示不使用负样本
    printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
    printf("\t-threads <int>\n");//训练时使用线程数,默认为12
    printf("\t\tUse <int> threads (default 12)\n");
    printf("\t-iter <int>\n");//训练迭代次数,默认是5
    printf("\t\tRun more training iterations (default 5)\n");
    printf("\t-min-count <int>\n");//词出现次数下限,如果小于这个阈值,删除这个词(处理低频词);默认取值是5
    printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    printf("\t-alpha <float>\n");//学习率;skip-gram模型学习率默认是0.025;CBOW模型默认是0.05
    printf("\t\tSet the starting learning rate; default is 0.025 for skip-gram and 0.05 for CBOW\n");
    printf("\t-classes <int>\n");//输出词类别,而不是词向量;默认类别数目是0(输出词向量)
    printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
    printf("\t-debug <int>\n");//设置debug模型,默认是2,显示训练期间debug信息
    printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
    printf("\t-binary <int>\n");//是否以2进制形式保存词向量;默认是0(关闭,不以二进制形式保存)
    printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
    printf("\t-save-vocab <file>\n");//实值词典保存的文件
    printf("\t\tThe vocabulary will be saved to <file>\n");
    printf("\t-read-vocab <file>\n");//设置词典读取文件,不是从训练数据中构造的(已有,直接读取);
    printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    printf("\t-cbow <int>\n");//是否使用CBOW模型,默认是1[使用CBOW],如果是0[使用skip-gram模型]
    printf("\t\tUse the continuous bag of words model; default is 1 (use 0 for skip-gram model)\n");
    printf("\nExamples:\n");//运行实例
    printf("./word2vec -train data.txt -output vec.txt -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1 -iter 3\n\n");
    return 0;
  }
  output_file[0] = 0;//输出文件
  save_vocab_file[0] = 0;//输出词的文件
  read_vocab_file[0] = 0;//读入指定词的文件

  //解析word2vec所需要的参数
  if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);
  if (cbow) alpha = 0.05;
  if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);
  
  // Allocate the vocabulary table.存储词结构体的词典;vocab如果空间不够,会动态扩展
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  // 存储词的hash的数组
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  // sigmoid计算结果保存表; 申请数组大小为EXP_TABLE_SIZE+1,多一个 for safe
  expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
  /**预先计算sigmoid函数, sigmoid(x)=1/(1+exp(-x))=exp(x)/(1+exp(x));
   *为了解决低效率问题,对sigmoid函数作近似计算;
   * 我们发现:sigmoid(x)在x=0附近变化剧烈,往两边逐渐趋于平缓,
   * 当x<-6或x>6时函数值基本不变,前者趋于0,后者趋于1.
  */
  // sigmoid取值范围[-6, 6); 一共有EXP_TABLE_SIZE个点;
  for (i = 0; i < EXP_TABLE_SIZE; i++) {
    // 计算exp()
    expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
    expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
  }
  //模型训练
  TrainModel();

  return 0;
}
