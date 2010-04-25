/*  zpipe streaming file compressor v1.0

(C) 2009, Ocarina Networks, Inc.
    Written by Matt Mahoney, matmahoney@yahoo.com, Sept. 29, 2009.

    LICENSE

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 3 of
    the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details at
    Visit <http://www.gnu.org/copyleft/gpl.html>.

To compress:   zpipe c <input >output
To decompress: zpipe d <input >output

Compressed output is in ZPAQ format as one segment within one
block. The segment has no filename and no commment. It is readable
by other ZPAQ compatible decompressors. It is equivalent to:

  zpaq nicmid.cfg output input

Decompression will accept ZPAQ compressed files from any source,
including embedded in other data, such as self extracting archives.
If the input archive contains more than one file, then all of the
output is concatenated. It will exit if a checksum is present but
incorrect.

To compile:

g++ -O2 -march=pentiumpro -fomit-frame-pointer -s zpipe.cpp -o zpipe
To turn off assertion checking (faster), compile with -DNDEBUG

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h> // for setmode(), requires g++
#include <assert.h>

const int LEVEL=1;  // ZPAQ level 0=experimental 1=final

// 1, 2, 4 byte unsigned integers
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;

// Print an error message and exit
void error(const char* msg="") {
  fprintf(stderr, "\nError: %s\n", msg);
  exit(1);
}

// An Array of T is cleared and aligned on a 64 byte address
//   with no constructors called. No copy or assignment.
// Array<T> a(n, ex=0);  - creates n<<ex elements of type T
// a[i] - index
// a(i) - index mod n, n must be a power of 2
// a.size() - gets n
template <class T>
class Array {
private:
  T *data; // user location of [0] on a 64 byte boundary
  int n;   // user size-1
  int offset;  // distance back in bytes to start of actual allocation
  void operator=(const Array&);  // no assignment
  Array(const Array&);  // no copy
public:
  Array(int sz=0, int ex=0): data(0), n(-1), offset(0) {
    resize(sz, ex);} // [0..sz-1] = 0
  void resize(int sz, int ex=0); // change size, erase content to zeros
  ~Array() {resize(0);}  // free memory
  int size() const {return n+1;}  // get size
  T& operator[](int i) {assert(n>=0 && i>=0 && U32(i)<=U32(n)); return data[i];}
  T& operator()(int i) {assert(n>=0 && (n&(n+1))==0); return data[i&n];}
};

// Change size to sz<<ex elements of 0
template<class T>
void Array<T>::resize(int sz, int ex) {
  while (ex>0) {
    if (sz<0 || sz>=(1<<30)) fprintf(stderr, "Array too big\n"), exit(1);
    sz*=2, --ex;
  }
  if (sz<0) fprintf(stderr, "Array too big\n"), exit(1);
  if (n>-1) {
    assert(offset>0 && offset<=64);
    assert((char*)data-offset);
    free((char*)data-offset);
  }
  n=-1;
  if (sz<=0) return;
  n=sz-1;
  data=(T*)calloc(64+(n+1)*sizeof(T), 1);
  if (!data) fprintf(stderr, "Out of memory\n"), exit(1);
  offset=64-int((long)data&63);
  assert(offset>0 && offset<=64);
  data=(T*)((char*)data+offset);
}

//////////////////////////// SHA-1 //////////////////////////////

// The SHA1 class is used to compute segment checksums.
// SHA-1 code modified from RFC 3174.
// http://www.faqs.org/rfcs/rfc3174.html

enum
{
    shaSuccess = 0,
    shaNull,            /* Null pointer parameter */
    shaInputTooLong,    /* input data too long */
    shaStateError       /* called Input after Result */
};
const int SHA1HashSize=20;

class SHA1 {
  U32 Intermediate_Hash[SHA1HashSize/4]; /* Message Digest  */
  U32 Length_Low;            /* Message length in bits      */
  U32 Length_High;           /* Message length in bits      */
  int Message_Block_Index;   /* Index into message block array */
  U8 Message_Block[64];      /* 512-bit message blocks      */
  int Computed;              /* Is the digest computed?         */
  int Corrupted;             /* Is the message digest corrupted? */
  U8 result_buf[20];         // Place to put result
  void SHA1PadMessage();
  void SHA1ProcessMessageBlock();
  U32 SHA1CircularShift(int bits, U32 word) {
     return (((word) << (bits)) | ((word) >> (32-(bits))));
  }
  int SHA1Reset();   // Initalize
  int SHA1Input(const U8 *, unsigned int n);  // Hash n bytes
  int SHA1Result(U8 Message_Digest[SHA1HashSize]);  // Store result
public:
  SHA1() {SHA1Reset();}  // Begin hash
  void put(int c) {  // Hash 1 byte
    U8 ch=c;
    SHA1Input(&ch, 1);
  }
  int result(int i);  // Finish and return byte i (0..19) of SHA1 hash
};

int SHA1::result(int i) {
  assert(i>=0 && i<20);
  if (!Computed && shaSuccess != SHA1Result(result_buf))
    error("SHA1 failed\n");
  return result_buf[i];
}

/*
 *  SHA1Reset
 *
 *  Description:
 *      This function will initialize the SHA1Context in preparation
 *      for computing a new SHA1 message digest.
 *
 *  Parameters: none
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1::SHA1Reset()
{
    Length_Low             = 0;
    Length_High            = 0;
    Message_Block_Index    = 0;

    Intermediate_Hash[0]   = 0x67452301;
    Intermediate_Hash[1]   = 0xEFCDAB89;
    Intermediate_Hash[2]   = 0x98BADCFE;
    Intermediate_Hash[3]   = 0x10325476;
    Intermediate_Hash[4]   = 0xC3D2E1F0;

    Computed   = 0;
    Corrupted  = 0;

    return shaSuccess;
}

/*
 *  SHA1Result
 *
 *  Description:
 *      This function will return the 160-bit message digest into the
 *      Message_Digest array  provided by the caller.
 *      NOTE: The first octet of hash is stored in the 0th element,
 *            the last octet of hash in the 19th element.
 *
 *  Parameters:
 *      Message_Digest: [out]
 *          Where the digest is returned.
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1::SHA1Result(U8 Message_Digest[SHA1HashSize])
{
    int i;

    if (!Message_Digest)
    {
        return shaNull;
    }

    if (Corrupted)
    {
        return Corrupted;
    }

    if (!Computed)
    {
        SHA1PadMessage();
        for(i=0; i<64; ++i)
        {
            /* message may be sensitive, clear it out */
            Message_Block[i] = 0;
        }
        Length_Low = 0;    /* and clear length */
        Length_High = 0;
        Computed = 1;

    }

    for(i = 0; i < SHA1HashSize; ++i)
    {
        Message_Digest[i] = Intermediate_Hash[i>>2]
                            >> 8 * ( 3 - ( i & 0x03 ) );
    }

    return shaSuccess;
}

/*
 *  SHA1Input
 *
 *  Description:
 *      This function accepts an array of octets as the next portion
 *      of the message.
 *
 *  Parameters:
 *      message_array: [in]
 *          An array of characters representing the next portion of
 *          the message.
 *      length: [in]
 *          The length of the message in message_array
 *
 *  Returns:
 *      sha Error Code.
 *
 */
int SHA1::SHA1Input(const U8  *message_array, unsigned length)
{
    if (!length)
    {
        return shaSuccess;
    }

    if (!message_array)
    {
        return shaNull;
    }

    if (Computed)
    {
        Corrupted = shaStateError;

        return shaStateError;
    }

    if (Corrupted)
    {
         return Corrupted;
    }
    while(length-- && !Corrupted)
    {
    Message_Block[Message_Block_Index++] =
                    (*message_array & 0xFF);

    Length_Low += 8;
    if (Length_Low == 0)
    {
        Length_High++;
        if (Length_High == 0)
        {
            /* Message is too long */
            Corrupted = 1;
        }
    }

    if (Message_Block_Index == 64)
    {
        SHA1ProcessMessageBlock();
    }

    message_array++;
    }

    return shaSuccess;
}

/*
 *  SHA1ProcessMessageBlock
 *
 *  Description:
 *      This function will process the next 512 bits of the message
 *      stored in the Message_Block array.
 *
 *  Parameters:
 *      None.
 *
 *  Returns:
 *      Nothing.
 *
 *  Comments:

 *      Many of the variable names in this code, especially the
 *      single character names, were used because those were the
 *      names used in the publication.
 *
 *
 */
void SHA1::SHA1ProcessMessageBlock()
{
    const U32 K[] =    {       /* Constants defined in SHA-1   */
                            0x5A827999,
                            0x6ED9EBA1,
                            0x8F1BBCDC,
                            0xCA62C1D6
                            };
    int      t;                 /* Loop counter                */
    U32      temp;              /* Temporary word value        */
    U32      W[80];             /* Word sequence               */
    U32      A, B, C, D, E;     /* Word buffers                */

    /*
     *  Initialize the first 16 words in the array W
     */
    for(t = 0; t < 16; t++)
    {
        W[t] = Message_Block[t * 4] << 24;
        W[t] |= Message_Block[t * 4 + 1] << 16;
        W[t] |= Message_Block[t * 4 + 2] << 8;
        W[t] |= Message_Block[t * 4 + 3];
    }

    for(t = 16; t < 80; t++)
    {
       W[t] = SHA1CircularShift(1,W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]);
    }

    A = Intermediate_Hash[0];
    B = Intermediate_Hash[1];
    C = Intermediate_Hash[2];
    D = Intermediate_Hash[3];
    E = Intermediate_Hash[4];

    for(t = 0; t < 20; t++)
    {
        temp =  SHA1CircularShift(5,A) +
                ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);

        B = A;
        A = temp;
    }

    for(t = 20; t < 40; t++)
    {
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[1];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    for(t = 40; t < 60; t++)
    {
        temp = SHA1CircularShift(5,A) +
               ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    for(t = 60; t < 80; t++)
    {
        temp = SHA1CircularShift(5,A) + (B ^ C ^ D) + E + W[t] + K[3];
        E = D;
        D = C;
        C = SHA1CircularShift(30,B);
        B = A;
        A = temp;
    }

    Intermediate_Hash[0] += A;
    Intermediate_Hash[1] += B;
    Intermediate_Hash[2] += C;
    Intermediate_Hash[3] += D;
    Intermediate_Hash[4] += E;

    Message_Block_Index = 0;
}

/*
 *  SHA1PadMessage
 *

 *  Description:
 *      According to the standard, the message must be padded to an even
 *      512 bits.  The first padding bit must be a '1'.  The last 64
 *      bits represent the length of the original message.  All bits in
 *      between should be 0.  This function will pad the message
 *      according to those rules by filling the Message_Block array
 *      accordingly.  It will also call the ProcessMessageBlock function
 *      provided appropriately.  When it returns, it can be assumed that
 *      the message digest has been computed.
 *
 *  Parameters:
 *      ProcessMessageBlock: [in]
 *          The appropriate SHA*ProcessMessageBlock function
 *  Returns:
 *      Nothing.
 *
 */

void SHA1::SHA1PadMessage()
{
    /*
     *  Check to see if the current message block is too small to hold
     *  the initial padding bits and length.  If so, we will pad the
     *  block, process it, and then continue padding into a second
     *  block.
     */
    if (Message_Block_Index > 55)
    {
        Message_Block[Message_Block_Index++] = 0x80;
        while(Message_Block_Index < 64)
        {
            Message_Block[Message_Block_Index++] = 0;
        }

        SHA1ProcessMessageBlock();

        while(Message_Block_Index < 56)
        {
            Message_Block[Message_Block_Index++] = 0;
        }
    }
    else
    {
        Message_Block[Message_Block_Index++] = 0x80;
        while(Message_Block_Index < 56)
        {

            Message_Block[Message_Block_Index++] = 0;
        }
    }

    /*
     *  Store the message length as the last 8 octets
     */
    Message_Block[56] = Length_High >> 24;
    Message_Block[57] = Length_High >> 16;
    Message_Block[58] = Length_High >> 8;
    Message_Block[59] = Length_High;
    Message_Block[60] = Length_Low >> 24;
    Message_Block[61] = Length_Low >> 16;
    Message_Block[62] = Length_Low >> 8;
    Message_Block[63] = Length_Low;

    SHA1ProcessMessageBlock();
}

//////////////////////////// ZPAQL //////////////////////////////

// Symbolic constants, instruction size, and names
typedef enum {NONE,CONST,CM,ICM,MATCH,AVG,MIX2,MIX,ISSE,SSE} CompType;
static const int compsize[256]={0,2,3,2,3,4,6,6,3,5};

// A ZPAQL machine HCOMP or PCOMP.
class ZPAQL {
public:
  ZPAQL();
  void load(int cn, int hn, const U8* data); // init from data[cn+hn]
  void read(FILE* in);    // Read header from archive
  void write(FILE* out);  // Write header to archive
  void inith();           // Initialize as HCOMP
  void initp();           // Initialize as PCOMP
  void run(U32 input);    // Execute with input
  int ph() {return header[4];}  // ph
  int pm() {return header[5];}  // pm
  FILE* output;           // Destination for OUT instruction, or 0 to suppress
  SHA1* sha1;             // Points to checksum computer
  friend class Predictor;
  friend class PostProcessor;
private:

  // ZPAQ1 block header
  int hsize;          // Header size
  Array<U8> header;   // hsize[2] hh hm ph pm n COMP (guard) HCOMP (guard)
  int cend;           // COMP in header[7...cend-1] (empty for PCOMP)
  int hbegin, hend;   // HCOMP in header[hbegin...hend-1]

  // Machine state for executing HCOMP
  Array<U8> m;        // memory array M for HCOMP
  Array<U32> h;       // hash array H for HCOMP
  Array<U32> r;       // 256 element register array
  U32 a, b, c, d;     // machine registers
  int f;              // condition flag
  int pc;             // program counter

  // Support code
  void init(int hbits, int mbits);  // initialize H and M sizes
  int execute();  // execute 1 instruction, return 0 after HALT, else 1
  void div(U32 x) {if (x) a/=x; else a=0;}
  void mod(U32 x) {if (x) a%=x; else a=0;}
  void swap(U32& x) {a^=x; x^=a; a^=x;}
  void swap(U8& x)  {a^=x; x^=a; a^=x;}
  void err();  // exit with run time error
};

// Constructor
ZPAQL::ZPAQL() {
  hsize=cend=hbegin=hend=0;
  a=b=c=d=f=pc=0;
  output=0;
  sha1=0;
}

// Copy cn bytes of COMP and hn bytes of HCOMP from data to header
void ZPAQL::load(int cn, int hn, const U8* data) {
  assert(header.size()==0);
  assert(cn>=7);
  assert(hn>=1);
  assert(data);
  cend=cn;
  hbegin=cend+128;
  hend=hbegin+hn;
  header.resize(hend+144);
  for (int i=0; i<cn; ++i)
    header[i]=data[i];
  for (int i=0; i<hn; ++i)
    header[hbegin+i]=data[cn+i];
  hsize=cn+hn-2;
  assert(header[0]+256*header[1]==hsize);
  assert(header[cend-1]==0);
  assert(header[hend-1]==0);
}

// Read header
void ZPAQL::read(FILE* in) {
  assert(in);

  // Get header size and allocate
  hsize=getc(in);
  hsize+=getc(in)*256;
  header.resize(hsize+300);
  cend=hbegin=hend=0;
  header[cend++]=hsize&255;
  header[cend++]=hsize>>8;
  while (cend<7) header[cend++]=getc(in); // hh hm ph pm n

  // Read COMP
  int n=header[cend-1];
  for (int i=0; i<n; ++i) {
    int type=getc(in);  // component type
    if (type==EOF) error("unexpected end of file");
    header[cend++]=type;  // component type
    int size=compsize[type];
    if (size<1) error("Invalid component type");
    if (cend+size>header.size()-8) error("COMP list too big");
    for (int j=1; j<size; ++j)
      header[cend++]=getc(in);
  }
  if ((header[cend++]=getc(in))!=0) error("missing COMP END");

  // Insert a guard gap and read HCOMP
  hbegin=hend=cend+128;
  while (hend<hsize+129) {
    assert(hend<header.size()-8);
    int op=getc(in);
    if (op==EOF) error("unexpected end of file");
    header[hend++]=op;
  }
  if ((header[hend++]=getc(in))!=0) error("missing HCOMP END");

  assert(cend>=7 && cend<header.size());
  assert(hbegin==cend+128 && hbegin<header.size());
  assert(hend>hbegin && hend<header.size());
  assert(hsize==header[0]+256*header[1]);
  assert(hsize==cend-2+hend-hbegin);
}

// Write header
void ZPAQL::write(FILE* out) {
  assert(out);
  assert(cend>=7 && cend<header.size());
  assert(hbegin==cend+128 && hbegin<header.size());
  assert(hend>hbegin && hend<header.size());
  assert(hsize==header[0]+256*header[1]);
  assert(hsize==cend-2+hend-hbegin);
  fwrite(&header[0], 1, cend, out);
  fwrite(&header[hbegin], 1, hend-hbegin, out);
}

// Initialize machine state as HCOMP
void ZPAQL::inith() {
  init(header[2], header[3]); // hh, hm
}

// Initialize machine state as PCOMP
void ZPAQL::initp() {
  init(header[4], header[5]);  // ph, pm
}

// Initialize machine state
void ZPAQL::init(int hbits, int mbits) {
  assert(h.size()==0);
  assert(m.size()==0);
  assert(hbegin>=cend+128);
  assert(cend>=7);
  h.resize(1, hbits);
  m.resize(1, mbits);
  r.resize(256);
  a=b=c=d=pc=f=0;
}

// Run program on input
void ZPAQL::run(U32 input) {
  assert(cend>6);
  assert(hbegin==cend+128);
  assert(hend>hbegin);
  assert(hend<header.size()-130);
  assert(m.size()>0);
  assert(h.size()>0);
  pc=hbegin;
  a=input;
  while (execute()) ;
}

// Execute one instruction, return 0 after HALT else 1
inline int ZPAQL::execute() {
  switch(header[pc++]) {
    case 0: err(); break; // ERROR
    case 1: ++a; break; // A++
    case 2: --a; break; // A--
    case 3: a = ~a; break; // A!
    case 4: a = 0; break; // A=0
    case 7: a = r[header[pc++]]; break; // A=R N
    case 8: swap(b); break; // B<>A
    case 9: ++b; break; // B++
    case 10: --b; break; // B--
    case 11: b = ~b; break; // B!
    case 12: b = 0; break; // B=0
    case 15: b = r[header[pc++]]; break; // B=R N
    case 16: swap(c); break; // C<>A
    case 17: ++c; break; // C++
    case 18: --c; break; // C--
    case 19: c = ~c; break; // C!
    case 20: c = 0; break; // C=0
    case 23: c = r[header[pc++]]; break; // C=R N
    case 24: swap(d); break; // D<>A
    case 25: ++d; break; // D++
    case 26: --d; break; // D--
    case 27: d = ~d; break; // D!
    case 28: d = 0; break; // D=0
    case 31: d = r[header[pc++]]; break; // D=R N
    case 32: swap(m(b)); break; // *B<>A
    case 33: ++m(b); break; // *B++
    case 34: --m(b); break; // *B--
    case 35: m(b) = ~m(b); break; // *B!
    case 36: m(b) = 0; break; // *B=0
    case 39: if (f) pc+=((header[pc]+128)&255)-127; else ++pc; break; // JT N
    case 40: swap(m(c)); break; // *C<>A
    case 41: ++m(c); break; // *C++
    case 42: --m(c); break; // *C--
    case 43: m(c) = ~m(c); break; // *C!
    case 44: m(c) = 0; break; // *C=0
    case 47: if (!f) pc+=((header[pc]+128)&255)-127; else ++pc; break; // JF N
    case 48: swap(h(d)); break; // *D<>A
    case 49: ++h(d); break; // *D++
    case 50: --h(d); break; // *D--
    case 51: h(d) = ~h(d); break; // *D!
    case 52: h(d) = 0; break; // *D=0
    case 55: r[header[pc++]] = a; break; // R=A N
    case 56: return 0  ; // HALT
    case 57: if (output) putc(a, output); if (sha1) sha1->put(a); break; // OUT
    case 59: a = (a+m(b)+512)*773; break; // HASH
    case 60: h(d) = (h(d)+a+512)*773; break; // HASHD
    case 63: pc+=((header[pc]+128)&255)-127; break; // JMP N
    case 64: a = a; break; // A=A
    case 65: a = b; break; // A=B
    case 66: a = c; break; // A=C
    case 67: a = d; break; // A=D
    case 68: a = m(b); break; // A=*B
    case 69: a = m(c); break; // A=*C
    case 70: a = h(d); break; // A=*D
    case 71: a = header[pc++]; break; // A= N
    case 72: b = a; break; // B=A
    case 73: b = b; break; // B=B
    case 74: b = c; break; // B=C
    case 75: b = d; break; // B=D
    case 76: b = m(b); break; // B=*B
    case 77: b = m(c); break; // B=*C
    case 78: b = h(d); break; // B=*D
    case 79: b = header[pc++]; break; // B= N
    case 80: c = a; break; // C=A
    case 81: c = b; break; // C=B
    case 82: c = c; break; // C=C
    case 83: c = d; break; // C=D
    case 84: c = m(b); break; // C=*B
    case 85: c = m(c); break; // C=*C
    case 86: c = h(d); break; // C=*D
    case 87: c = header[pc++]; break; // C= N
    case 88: d = a; break; // D=A
    case 89: d = b; break; // D=B
    case 90: d = c; break; // D=C
    case 91: d = d; break; // D=D
    case 92: d = m(b); break; // D=*B
    case 93: d = m(c); break; // D=*C
    case 94: d = h(d); break; // D=*D
    case 95: d = header[pc++]; break; // D= N
    case 96: m(b) = a; break; // *B=A
    case 97: m(b) = b; break; // *B=B
    case 98: m(b) = c; break; // *B=C
    case 99: m(b) = d; break; // *B=D
    case 100: m(b) = m(b); break; // *B=*B
    case 101: m(b) = m(c); break; // *B=*C
    case 102: m(b) = h(d); break; // *B=*D
    case 103: m(b) = header[pc++]; break; // *B= N
    case 104: m(c) = a; break; // *C=A
    case 105: m(c) = b; break; // *C=B
    case 106: m(c) = c; break; // *C=C
    case 107: m(c) = d; break; // *C=D
    case 108: m(c) = m(b); break; // *C=*B
    case 109: m(c) = m(c); break; // *C=*C
    case 110: m(c) = h(d); break; // *C=*D
    case 111: m(c) = header[pc++]; break; // *C= N
    case 112: h(d) = a; break; // *D=A
    case 113: h(d) = b; break; // *D=B
    case 114: h(d) = c; break; // *D=C
    case 115: h(d) = d; break; // *D=D
    case 116: h(d) = m(b); break; // *D=*B
    case 117: h(d) = m(c); break; // *D=*C
    case 118: h(d) = h(d); break; // *D=*D
    case 119: h(d) = header[pc++]; break; // *D= N
    case 128: a += a; break; // A+=A
    case 129: a += b; break; // A+=B
    case 130: a += c; break; // A+=C
    case 131: a += d; break; // A+=D
    case 132: a += m(b); break; // A+=*B
    case 133: a += m(c); break; // A+=*C
    case 134: a += h(d); break; // A+=*D
    case 135: a += header[pc++]; break; // A+= N
    case 136: a -= a; break; // A-=A
    case 137: a -= b; break; // A-=B
    case 138: a -= c; break; // A-=C
    case 139: a -= d; break; // A-=D
    case 140: a -= m(b); break; // A-=*B
    case 141: a -= m(c); break; // A-=*C
    case 142: a -= h(d); break; // A-=*D
    case 143: a -= header[pc++]; break; // A-= N
    case 144: a *= a; break; // A*=A
    case 145: a *= b; break; // A*=B
    case 146: a *= c; break; // A*=C
    case 147: a *= d; break; // A*=D
    case 148: a *= m(b); break; // A*=*B
    case 149: a *= m(c); break; // A*=*C
    case 150: a *= h(d); break; // A*=*D
    case 151: a *= header[pc++]; break; // A*= N
    case 152: div(a); break; // A/=A
    case 153: div(b); break; // A/=B
    case 154: div(c); break; // A/=C
    case 155: div(d); break; // A/=D
    case 156: div(m(b)); break; // A/=*B
    case 157: div(m(c)); break; // A/=*C
    case 158: div(h(d)); break; // A/=*D
    case 159: div(header[pc++]); break; // A/= N
    case 160: mod(a); break; // A%=A
    case 161: mod(b); break; // A%=B
    case 162: mod(c); break; // A%=C
    case 163: mod(d); break; // A%=D
    case 164: mod(m(b)); break; // A%=*B
    case 165: mod(m(c)); break; // A%=*C
    case 166: mod(h(d)); break; // A%=*D
    case 167: mod(header[pc++]); break; // A%= N
    case 168: a &= a; break; // A&=A
    case 169: a &= b; break; // A&=B
    case 170: a &= c; break; // A&=C
    case 171: a &= d; break; // A&=D
    case 172: a &= m(b); break; // A&=*B
    case 173: a &= m(c); break; // A&=*C
    case 174: a &= h(d); break; // A&=*D
    case 175: a &= header[pc++]; break; // A&= N
    case 176: a &= ~ a; break; // A&~A
    case 177: a &= ~ b; break; // A&~B
    case 178: a &= ~ c; break; // A&~C
    case 179: a &= ~ d; break; // A&~D
    case 180: a &= ~ m(b); break; // A&~*B
    case 181: a &= ~ m(c); break; // A&~*C
    case 182: a &= ~ h(d); break; // A&~*D
    case 183: a &= ~ header[pc++]; break; // A&~ N
    case 184: a |= a; break; // A|=A
    case 185: a |= b; break; // A|=B
    case 186: a |= c; break; // A|=C
    case 187: a |= d; break; // A|=D
    case 188: a |= m(b); break; // A|=*B
    case 189: a |= m(c); break; // A|=*C
    case 190: a |= h(d); break; // A|=*D
    case 191: a |= header[pc++]; break; // A|= N
    case 192: a ^= a; break; // A^=A
    case 193: a ^= b; break; // A^=B
    case 194: a ^= c; break; // A^=C
    case 195: a ^= d; break; // A^=D
    case 196: a ^= m(b); break; // A^=*B
    case 197: a ^= m(c); break; // A^=*C
    case 198: a ^= h(d); break; // A^=*D
    case 199: a ^= header[pc++]; break; // A^= N
    case 200: a <<= a; break; // A<<=A
    case 201: a <<= b; break; // A<<=B
    case 202: a <<= c; break; // A<<=C
    case 203: a <<= d; break; // A<<=D
    case 204: a <<= m(b); break; // A<<=*B
    case 205: a <<= m(c); break; // A<<=*C
    case 206: a <<= h(d); break; // A<<=*D
    case 207: a <<= header[pc++]; break; // A<<= N
    case 208: a >>= a; break; // A>>=A
    case 209: a >>= b; break; // A>>=B
    case 210: a >>= c; break; // A>>=C
    case 211: a >>= d; break; // A>>=D
    case 212: a >>= m(b); break; // A>>=*B
    case 213: a >>= m(c); break; // A>>=*C
    case 214: a >>= h(d); break; // A>>=*D
    case 215: a >>= header[pc++]; break; // A>>= N
    case 216: f = (a == a); break; // A==A
    case 217: f = (a == b); break; // A==B
    case 218: f = (a == c); break; // A==C
    case 219: f = (a == d); break; // A==D
    case 220: f = (a == U32(m(b))); break; // A==*B
    case 221: f = (a == U32(m(c))); break; // A==*C
    case 222: f = (a == h(d)); break; // A==*D
    case 223: f = (a == U32(header[pc++])); break; // A== N
    case 224: f = (a < a); break; // A<A
    case 225: f = (a < b); break; // A<B
    case 226: f = (a < c); break; // A<C
    case 227: f = (a < d); break; // A<D
    case 228: f = (a < U32(m(b))); break; // A<*B
    case 229: f = (a < U32(m(c))); break; // A<*C
    case 230: f = (a < h(d)); break; // A<*D
    case 231: f = (a < U32(header[pc++])); break; // A< N
    case 232: f = (a > a); break; // A>A
    case 233: f = (a > b); break; // A>B
    case 234: f = (a > c); break; // A>C
    case 235: f = (a > d); break; // A>D
    case 236: f = (a > U32(m(b))); break; // A>*B
    case 237: f = (a > U32(m(c))); break; // A>*C
    case 238: f = (a > h(d)); break; // A>*D
    case 239: f = (a > U32(header[pc++])); break; // A> N
    case 255: if((pc=hbegin+header[pc]+256*header[pc+1])>=hend)err();break;//LJ
    default: err();
  }
  return 1;
}

// Print illegal instruction error message and exit
void ZPAQL::err() {
  error("zpaql execution");
}

///////////////////////////// Predictor ///////////////////////////

// A Component represents state information used to map a context
// and other component outputs to a bit prediction.

struct Component {
  int limit;      // max count for cm
  U32 cxt;        // saved context
  int a, b, c;    // multi-purpose variables
  Array<U32> cm;  // cm[cxt] -> p in bits 31..10, n in 9..0; MATCH index
  Array<U8> ht;   // ICM hash table[0..size1][0..15] of bit histories; MATCH buf
  Array<U16> a16; // multi-use
  Component();    // initialize to all 0
};

Component::Component(): limit(0), cxt(0), a(0), b(0), c(0) {}

// A StateTable generates a table that maps a bit history and a bit
// to an updated history, and maps a history to the 0,1 counts it represents.

class StateTable {
  enum {B=6, N=64}; // sizes of b, t
  static U8 ns[1024]; // state*4 -> next state if 0, if 1, n0, n1
  static const int bound[B];  // n0 -> max n1, n1 -> max n0
  int num_states(int n0, int n1);  // compute t[n0][n1][1]
  void discount(int& n0);  // set new value of n0 after 1 or n1 after 0
  void next_state(int& n0, int& n1, int y);  // new (n0,n1) after bit y
public:
  // next(s, 0) -> next state if 0, s in (0..255), result in (0..255)
  // next(s  1) -> next state if 1
  // next(s, 2) -> zero count represented by s
  // next(s, 3) -> one count represented by s
  int next(int state, int y) {
    assert(state>=0 && state<256);
    assert(y>=0 && y<4);
    return ns[state*4+y];
  }
  int cminit(int state) {  // initial probability of 1 * 2^23
    assert(state>=0 && state<256);
    return ((ns[state*4+3]*2+1)<<22)/(ns[state*4+2]+ns[state*4+3]+1);
  }
  StateTable();
};

U8 StateTable::ns[1024]={0};
const int StateTable::bound[B]={20,48,15,8,6,5}; // n0 -> max n1, n1 -> max n0

// How many states with count of n0 zeros, n1 ones (0...2)
int StateTable::num_states(int n0, int n1) {
  if (n0<n1) return num_states(n1, n0);
  if (n0<0 || n1<0 || n0>=N || n1>=N || n1>=B || n0>bound[n1]) return 0;
  return 1+(n1>0 && n0+n1<=17);
}

// New value of count n0 if 1 is observed (and vice versa)
void StateTable::discount(int& n0) {
  n0=(n0>=1)+(n0>=2)+(n0>=3)+(n0>=4)+(n0>=5)+(n0>=7)+(n0>=8);
}

// compute next n0,n1 (0 to N) given input y (0 or 1)
void StateTable::next_state(int& n0, int& n1, int y) {
  if (n0<n1)
    next_state(n1, n0, 1-y);
  else {
    if (y) {
      ++n1;
      discount(n0);
    }
    else {
      ++n0;
      discount(n1);
    }
    // 20,0,0 -> 20,0
    // 48,1,0 -> 48,1
    // 15,2,0 -> 8,1
    //  8,3,0 -> 6,2
    //  8,3,1 -> 5,3
    //  6,4,0 -> 5,3
    //  5,5,0 -> 5,4
    //  5,5,1 -> 4,5
    while (!num_states(n0, n1)) {
      if (n1<2) --n0;
      else {
        n0=(n0*(n1-1)+(n1/2))/n1;
        --n1;
      }
    }
  }
}

// Initialize next state table ns[state*4] -> next if 0, next if 1, n0, n1
StateTable::StateTable() {

  // Assign states by increasing priority
  U8 t[N][N][2]={{{0}}}; // (n0,n1,y) -> state number
  int state=0;
  for (int i=0; i<N; ++i) {
    for (int n1=0; n1<=i; ++n1) {
      int n0=i-n1;
      int n=num_states(n0, n1);
      assert(n>=0 && n<=2);
      if (n) {
        t[n0][n1][0]=state;
        t[n0][n1][1]=state+n-1;
        state+=n;
      }
    }
  }
       
  // Generate next state table
  for (int n0=0; n0<N; ++n0) {
    for (int n1=0; n1<N; ++n1) {
      for (int y=0; y<num_states(n0, n1); ++y) {
        int s=t[n0][n1][y];
        assert(s>=0 && s<256);
        int s0=n0, s1=n1;
        next_state(s0, s1, 0);
        assert(s0>=0 && s0<N && s1>=0 && s1<N);
        ns[s*4+0]=t[s0][s1][0];
        s0=n0, s1=n1;
        next_state(s0, s1, 1);
        assert(s0>=0 && s0<N && s1>=0 && s1<N);
        ns[s*4+1]=t[s0][s1][1];
        ns[s*4+2]=n0;
        ns[s*4+3]=n1;
      }
    }
  }
}

// A Predictor guesses the next bit. The constructor builds a model
// according to the instructions given in the ZPAQL code in the
// block header. predict() returns p1 in (0..32767) such that the
// probability that the next bit is a 1 is (p1*2+1)/65536. update(y)
// updates the models with actual bit y (0..1) to reduce the prediction
// errors of individual components.

class Predictor {
public:
  Predictor(ZPAQL&);    // build model
  int predict();        // probability that next bit is a 1 (0..32767)
  void update(int y);   // train on bit y (0..1)
  void stat();          // print statistics
private:

  // Predictor state
  int c8;               // last 0...7 bits.
  int hmap4;            // c8 split into nibbles
  int p[256];           // predictions
  ZPAQL& z;             // VM to compute context hashes, includes H, n
  Component comp[256];  // the model, includes P

  // Modeling support functions
  void train(Component& cr, int y);  // reduce prediction error in cr.cm
  int dt[1024];         // division table for cm: dt[i] = 2^16/(i+1.5)
  U16 squasht[4096];    // squash() lookup table
  short stretcht[32768];// stretch() lookup table
  StateTable st;        // next, cminit functions

  // x -> floor(32768/(1+exp(-x/64)))
  int squash(int x) {
    assert(x>=-2048 && x<=2047);
    return squasht[x+2048];
  }

  // x -> round(64*log((x+0.5)/(32767.5-x))), approx inverse of squash
  int stretch(int x) {
    assert(x>=0 && x<=32767);
    return stretcht[x];
  }

  // bound x to a 12 bit signed int
  int clamp2k(int x) {
    if (x<-2048) return -2048;
    else if (x>2047) return 2047;
    else return x;
  }

  // bound x to a 20 bit signed int
  int clamp512k(int x) {
    if (x<-(1<<19)) return -(1<<19);
    else if (x>=(1<<19)) return (1<<19)-1;
    else return x;
  }

  // Get cxt in ht, creating a new row if needed
  int find(Array<U8>& ht, int sizebits, U32 cxt);
};

// Initailize the model
Predictor::Predictor(ZPAQL& zr): c8(1), hmap4(1), z(zr) {
  assert(sizeof(U8)==1);
  assert(sizeof(U16)==2);
  assert(sizeof(U32)==4);
  assert(sizeof(short)==2);
  assert(sizeof(int)==4);

  // Initialize tables
  for (int i=0; i<1024; ++i)
    dt[i]=(1<<17)/(i*2+3)*2;
  for (int i=0; i<32768; ++i)
    stretcht[i]=int(log((i+0.5)/(32767.5-i))*64+0.5+100000)-100000;
  for (int i=0; i<4096; ++i)
    squasht[i]=int(32768.0/(1+exp((i-2048)*(-1.0/64))));

  // Verify floating point math for squash() and stretch()
  U32 sqsum=0, stsum=0;
  for (int i=32767; i>=0; --i)
    stsum=stsum*3+stretch(i);
  for (int i=4095; i>=0; --i)
    sqsum=sqsum*3+squash(i-2048);
  assert(stsum==3887533746u);
  assert(sqsum==2278286169u);

  // Initialize context hash function
  z.inith();

  // Initialize predictions
  for (int i=0; i<256; ++i) p[i]=0;

  // Initialize components
  int n=z.header[6]; // hsize[0..1] hh hm ph pm n (comp)[n] END 0[128] (hcomp) END
  if (n<1 || n>255) error("n must be 1..255 components");
  const U8* cp=&z.header[7];  // start of component list
  for (int i=0; i<n; ++i) {
    assert(cp<&z.header[z.cend]);
    assert(cp>&z.header[0] && cp<&z.header[z.header.size()-8]);
    Component& cr=comp[i];
    switch(cp[0]) {
      case CONST:  // c
        p[i]=(cp[1]-128)*4;
        break;
      case CM: // sizebits limit
        cr.cm.resize(1, cp[1]);  // packed CM (22 bits) + CMCOUNT (10 bits)
        cr.limit=cp[2]*4;
        for (int j=0; j<cr.cm.size(); ++j)
          cr.cm[j]=0x80000000;
        break;
      case ICM: // sizebits
        cr.limit=1023;
        cr.cm.resize(256);
        cr.ht.resize(64, cp[1]);
        for (int j=0; j<cr.cm.size(); ++j)
          cr.cm[j]=st.cminit(j);
        break;
      case MATCH:  // sizebits
        cr.cm.resize(1, cp[1]);  // index
        cr.ht.resize(1, cp[2]);  // buf
        cr.ht(0)=1;
        break;
      case AVG: // j k wt
        break;
      case MIX2:  // sizebits j k rate mask
        if (cp[3]>=i) error("MIX2 k >= i");
        if (cp[2]>=i) error("MIX2 j >= i");
        cr.c=(1<<cp[1]); // size (number of contexts)
        cr.a16.resize(1, cp[1]);  // wt[size][m]
        for (int j=0; j<cr.a16.size(); ++j)
          cr.a16[j]=32768;
        break;
      case MIX: {  // sizebits j m rate mask
        if (cp[2]>=i) error("MIX j >= i");
        if (cp[3]<1 || cp[3]>i-cp[2])
          error("MIX m not in 1..i-j");
        int m=cp[3];  // number of inputs
        assert(m>=1);
        cr.c=(1<<cp[1]); // size (number of contexts)
        cr.cm.resize(m, cp[1]);  // wt[size][m]
        for (int j=0; j<cr.cm.size(); ++j)
          cr.cm[j]=65536/m;
        break;
      }
      case ISSE:  // sizebits j
        if (cp[2]>=i) error("ISSE j >= i");
        cr.ht.resize(64, cp[1]);
        cr.cm.resize(512);
        for (int j=0; j<256; ++j) {
          cr.cm[j*2]=1<<15;
          cr.cm[j*2+1]=clamp512k(stretch(st.cminit(j)>>8)<<10);
        }
        break;
      case SSE: // sizebits j start limit
        if (cp[2]>=i) error("SSE j >= i");
        if (cp[3]>cp[4]*4) error("SSE start > limit*4");
        cr.cm.resize(32, cp[1]);
        cr.limit=cp[4]*4;
        for (int j=0; j<cr.cm.size(); ++j)
          cr.cm[j]=squash((j&31)*64-992)<<17|cp[3];
        break;
      default: error("unknown component type");
    }
    assert(compsize[*cp]>0);
    cp+=compsize[*cp];
    assert(cp>=&z.header[7] && cp<&z.header[z.cend]);
  }
}

// Return next bit prediction (0..32767)
int Predictor::predict() {
  assert(c8>=1 && c8<=255);

  // Predict next bit
  int n=z.header[6];
  assert(n>0 && n<=255);
  const U8* cp=&z.header[7];
  assert(cp[-1]==n);
  for (int i=0; i<n; ++i) {
    assert(cp>&z.header[0] && cp<&z.header[z.header.size()-8]);
    Component& cr=comp[i];
    switch(cp[0]) {
      case CONST:  // c
        break;
      case CM:  // sizebits limit
        cr.cxt=z.h(i)^hmap4;
        p[i]=stretch(cr.cm(cr.cxt)>>17);
        break;
      case ICM: // sizebits
        assert((hmap4&15)>0);
        if (c8==1 || (c8&0xf0)==16) cr.c=find(cr.ht, cp[1]+2, z.h(i)+16*c8);
        cr.cxt=cr.ht[cr.c+(hmap4&15)];
        p[i]=stretch(cr.cm(cr.cxt)>>8);
        break;
      case MATCH: // sizebits bufbits: a=len, b=offset, c=bit, cxt=256/len,
                  //                   ht=buf, limit=8*pos+bp
        assert(cr.a>=0 && cr.a<=255);
        if (cr.a==0) p[i]=0;
        else {
          cr.c=cr.ht((cr.limit>>3)-cr.b)>>(7-(cr.limit&7))&1; // predicted bit
          p[i]=stretch(cr.cxt*(cr.c*-2+1)&32767);
        }
        break;
      case AVG: // j k wt
        p[i]=(p[cp[1]]*cp[3]+p[cp[2]]*(256-cp[3]))>>8;
        break;
      case MIX2: { // sizebits j k rate mask
                   // c=size cm=wt[size][m] cxt=input
        cr.cxt=((z.h(i)+(c8&cp[5]))&(cr.c-1));
        assert(int(cr.cxt)>=0 && int(cr.cxt)<cr.a16.size());
        int w=cr.a16[cr.cxt];
        assert(w>=0 && w<65536);
        p[i]=(w*p[cp[2]]+(65536-w)*p[cp[3]])>>16;
        assert(p[i]>=-2048 && p[i]<2048);
      }
        break;
      case MIX: {  // sizebits j m rate mask
                   // c=size cm=wt[size][m] cxt=index of wt in cm
        int m=cp[3];
        assert(m>=1 && m<=i);
        cr.cxt=z.h(i)+(c8&cp[5]);
        cr.cxt=(cr.cxt&(cr.c-1))*m; // pointer to row of weights
        assert(int(cr.cxt)>=0 && int(cr.cxt)<=cr.cm.size()-m);
        int* wt=(int*)&cr.cm[cr.cxt];
        p[i]=0;
        for (int j=0; j<m; ++j)
          p[i]+=(wt[j]>>8)*p[cp[2]+j];
        p[i]=clamp2k(p[i]>>8);
      }
        break;
      case ISSE: { // sizebits j -- c=hi, cxt=bh
        assert((hmap4&15)>0);
        if (c8==1 || (c8&0xf0)==16)
          cr.c=find(cr.ht, cp[1]+2, z.h(i)+16*c8);
        cr.cxt=cr.ht[cr.c+(hmap4&15)];  // bit history
        int *wt=(int*)&cr.cm[cr.cxt*2];
        p[i]=clamp2k((wt[0]*p[cp[2]]+wt[1]*64)>>16);
      }
        break;
      case SSE: { // sizebits j start limit
        cr.cxt=(z.h(i)+c8)*32;
        int pq=p[cp[2]]+992;
        if (pq<0) pq=0;
        if (pq>1983) pq=1983;
        int wt=pq&63;
        pq>>=6;
        assert(pq>=0 && pq<=30);
        cr.cxt+=pq;
        p[i]=stretch(((cr.cm(cr.cxt)>>10)*(64-wt)+(cr.cm(cr.cxt+1)>>10)*wt)>>13);
        cr.cxt+=wt>>5;
      }
        break;
      default:
        error("component predict not implemented");
    }
    cp+=compsize[cp[0]];
    assert(cp<&z.header[z.cend]);
    assert(p[i]>=-2048 && p[i]<2048);
  }
  assert(cp[0]==NONE);
  return squash(p[n-1]);
}

// Update model with decoded bit y (0...1)
void Predictor::update(int y) {
  assert(y==0 || y==1);
  assert(c8>=1 && c8<=255);
  assert(hmap4>=1 && hmap4<=511);

  // Update components
  const U8* cp=&z.header[7];
  int n=z.header[6];
  assert(n>=1 && n<=255);
  assert(cp[-1]==n);
  for (int i=0; i<n; ++i) {
    Component& cr=comp[i];
    switch(cp[0]) {
      case CONST:  // c
        break;
      case CM:  // sizebits limit
        train(cr, y);
        break;
      case ICM: { // sizebits: cxt=ht[b]=bh, ht[c][0..15]=bh row, cxt=bh
        cr.ht[cr.c+(hmap4&15)]=st.next(cr.ht[cr.c+(hmap4&15)], y);
        U32& pn=cr.cm(cr.cxt);
        pn+=int(y*32767-(pn>>8))>>2;
      }
        break;
      case MATCH: // sizebits bufbits:
                  //   a=len, b=offset, c=bit, cm=index, cxt=256/len
                  //   ht=buf, limit=8*pos+bp
      {
        assert(cr.a>=0 && cr.a<=255);
        assert(cr.c==0 || cr.c==1);
        if (cr.c!=y) cr.a=0;  // mismatch?
        cr.ht(cr.limit>>3)+=cr.ht(cr.limit>>3)+y;
        if ((++cr.limit&7)==0) {
          int pos=cr.limit>>3;
          if (cr.a==0) {  // look for a match
            cr.b=pos-cr.cm(z.h(i));
            if (cr.b&(cr.ht.size()-1))
              while (cr.a<255 && cr.ht(pos-cr.a-1)==cr.ht(pos-cr.a-cr.b-1))
                ++cr.a;
          }
          else cr.a+=cr.a<255;
          cr.cm(z.h(i))=pos;
          if (cr.a>0) cr.cxt=2048/cr.a;
        }
      }
        break;
      case AVG:  // j k wt
        break;
      case MIX2: { // sizebits j k rate mask
                   // cm=input[2],wt[size][2], cxt=weight row
        assert(cr.a16.size()==cr.c);
        assert(int(cr.cxt)>=0 && int(cr.cxt)<cr.a16.size());
        int err=(y*32767-squash(p[i]))*cp[4]>>5;
        int w=cr.a16[cr.cxt];
        w+=(err*(p[cp[2]]-p[cp[3]])+(1<<12))>>13;
        if (w<0) w=0;
        if (w>65535) w=65535;
        cr.a16[cr.cxt]=w;
      }
        break;
      case MIX: {   // sizebits j m rate mask
                    // cm=wt[size][m], cxt=input
        int m=cp[3];
        assert(m>0 && m<=i);
        assert(cr.cm.size()==m*cr.c);
        assert(int(cr.cxt)>=0 && int(cr.cxt)<=cr.cm.size()-m);
        int err=(y*32767-squash(p[i]))*cp[4]>>4;
        int* wt=(int*)&cr.cm[cr.cxt];
        for (int j=0; j<m; ++j)
          wt[j]=clamp512k(wt[j]+((err*p[cp[2]+j]+(1<<12))>>13));
      }
        break;
      case ISSE: { // sizebits j  -- c=hi, cxt=bh
        assert(cr.cxt==cr.ht[cr.c+(hmap4&15)]);
        int err=y*32767-squash(p[i]);
        int *wt=(int*)&cr.cm[cr.cxt*2];
        wt[0]=clamp512k(wt[0]+((err*p[cp[2]]+(1<<12))>>13));
        wt[1]=clamp512k(wt[1]+((err+16)>>5));
        cr.ht[cr.c+(hmap4&15)]=st.next(cr.cxt, y);
      }
        break;
      case SSE:  // sizebits j start limit
        train(cr, y);
        break;
      default:
        assert(0);
    }
    cp+=compsize[cp[0]];
    assert(cp>=&z.header[7] && cp<&z.header[z.cend] 
           && cp<&z.header[z.header.size()-8]);
  }
  assert(cp[0]==NONE);

  // Save bit y in c8, hmap4
  c8+=c8+y;
  if (c8>=256) {
    z.run(c8-256);
    hmap4=1;
    c8=1;
  }
  else if (c8>=16 && c8<32)
    hmap4=(hmap4&0xf)<<5|y<<4|1;
  else
    hmap4=(hmap4&0x1f0)|(((hmap4&0xf)*2+y)&0xf);
}

// cr.cm(cr.cxt) has a prediction in the high 22 bits and a count in the
// low 10 bits.  Reduce the prediction error by error/(count+1.5) and
// count up to cr.limit. cm.size() must be a power of 2.
inline void Predictor::train(Component& cr, int y) {
  assert(y==0 || y==1);
  U32& pn=cr.cm(cr.cxt);
  int count=pn&0x3ff;
  int error=y*32767-(cr.cm(cr.cxt)>>17);
  pn+=(error*dt[count]&-1024)+(count<cr.limit);
}

// Find cxt row in hash table ht. ht has rows of 16 indexed by the
// low sizebits of cxt with element 0 having the next higher 8 bits for
// collision detection. If not found after 3 adjacent tries, replace the
// row with lowest element 1 as priority. Return index of row.
int Predictor::find(Array<U8>& ht, int sizebits, U32 cxt) {
  assert(ht.size()==16<<sizebits);
  int chk=cxt>>sizebits&255;
  int h0=(cxt*16)&(ht.size()-16);
  if (ht[h0]==chk) return h0;
  int h1=h0^16;
  if (ht[h1]==chk) return h1;
  int h2=h0^32;
  if (ht[h2]==chk) return h2;
  if (ht[h0+1]<=ht[h1+1] && ht[h0+1]<=ht[h2+1])
    return memset(&ht[h0], 0, 16), ht[h0]=chk, h0;
  else if (ht[h1+1]<ht[h2+1])
    return memset(&ht[h1], 0, 16), ht[h1]=chk, h1;
  else
    return memset(&ht[h2], 0, 16), ht[h2]=chk, h2;
}

////////////////////////////// Decoder ////////////////////////////

// Decoder decompresses using an arithmetic code. Decoder(f, z)
// specifies output destination file f and a predictor model specified
// by ZPAQL header z. decompress() returns one decompressed byte (0..255)
// or EOF (-1) at the end of the compressed stream.

class Decoder {
  FILE* in;  // destination
  U32 low, high; // range
  U32 curr;  // last 4 bytes of archive
  Predictor pr;  // to get p
  int decode(int p); // return decoded bit (0..1) with probability p (0..65535)
public:
  Decoder(FILE* f, ZPAQL& z);
  int decompress();  // return a byte or EOF
};

Decoder::Decoder(FILE* f, ZPAQL& z):
  in(f), low(1), high(0xFFFFFFFF), curr(0), pr(z) {}

inline int Decoder::decode(int p) {
  assert(p>=0 && p<65536);
  assert(high>low && low>0);
  if (curr<low) error("archive corrupted");
  assert (curr>=low && curr<=high);
  U32 mid=low+((high-low)>>16)*p+((((high-low)&0xffff)*p)>>16); // split range
  assert(high>mid && mid>=low);
  int y=curr<=mid;
  if (y) high=mid; else low=mid+1; // pick half
  while ((high^low)<0x1000000) { // shift out identical leading bytes
    high=high<<8|255;
    low=low<<8;
    low+=(low==0);
    int c=getc(in);
    if (c==EOF) error("unexpected end of file");
    curr=curr<<8|c;
  }
  return y;
}

// Decompress 1 byte as 9 bits 0xxxxxxxx or EOF as 1. Model p(1) for
// the first bit as 0, which codes to 32 bits.
int Decoder::decompress() {
  if (curr==0) {  // finish initialization
    for (int i=0; i<4; ++i)
      curr=curr<<8|getc(in);
  }
  if (decode(0)) {
    if (curr!=0) error("decoding end of stream");
    return EOF;
  }
  else {
    int c=1;
    while (c<256) {  // get 8 bits
      int p=pr.predict()*2+1;
      c+=c+decode(p);
      pr.update(c&1);
    }
    return c-256;
  }
}

/////////////////////////// PostProcessor ////////////////////

// A PostProcessor feeds the decoded output to a ZPAQL program
// stored in the output header and executes the program with the
// rest of the decoded output as input to the program. The
// PostProcessor output is the output of this program. Also, compute
// the SHA1 hash of the output and save it in an SHA1 object.

class PostProcessor {
  int state;   // input parse state
  int ph, pm;  // sizes of H and M in z
  ZPAQL z;     // holds PCOMP
public:
  PostProcessor(ZPAQL& hz);
  void set(FILE* out, SHA1* p) {z.output=out; z.sha1=p;}  // Set output
  void write(int c);  // Input a byte
};

// Copy ph, pm from block header
PostProcessor::PostProcessor(ZPAQL& hz) {
  state=0;
  ph=hz.header[4];
  pm=hz.header[5];
}

// (PASS=0 | PROG=1 psize[0..1] pcomp[0..psize-1]) data... EOB=-1
void PostProcessor::write(int c) {
  assert(c>=-1 && c<=255);
  switch (state) {
    case 0:  // initial state
      if (c<0) error("Unexpected EOS");
      state=c+1;  // 1=PASS, 2=PROG
      if (state>2) error("unknown post processing type");
      break;
    case 1:  // PASS
      if (z.output && c>=0) putc(c, z.output);  // data
      if (z.sha1 && c>=0) z.sha1->put(c);
      break;
    case 2: // PROG
      if (c<0) error("Unexpected EOS");
      z.hsize=c;  // low byte of psize
      state=3;
      break;
    case 3:  // PROG psize[0]
      if (c<0) error("Unexpected EOS");
      z.hsize+=c*256+1;  // high byte of psize
      z.header.resize(z.hsize+300);
      z.cend=8;
      z.hbegin=z.hend=136;
      z.header[0]=z.hsize&255;
      z.header[1]=z.hsize>>8;
      z.header[4]=ph;
      z.header[5]=pm;
      state=4;
      break;
    case 4:  // PROG psize[0..1] pcomp[0...]
      if (c<0) error("Unexpected EOS");
      assert(z.hend<z.header.size());
      z.header[z.hend++]=c;  // one byte of pcomp
      if (z.hend-z.hbegin==z.hsize-1) {  // last byte of pcomp?
        z.header[z.hend++]=0;
        z.initp();
        state=5;
      }
      break;
    case 5:  // PROG ... data
      z.run(c);
      break;
  }
}

/////////////////////////// Decompress ///////////////////////


// Advance 'in' past "zPQ" at its current location. If something
// else is there, search for the following 16 byte string
// which ends with "zPQ":
// 37 6B 53 74  A0 31 83 D3  8C B2 28 B0  D3 7A 50 51 (hex)
// Return true if found, false at EOF.
bool find_start(FILE *in) {
  U32 h1=0x3D49B113, h2=0x29EB7F93, h3=0x2614BE13, h4=0x3828EB13;
  // Rolling hashes initialized to hash of first 13 bytes
  int c;
  while ((c=getc(in))!=EOF) {
    h1=h1*12+c;
    h2=h2*20+c;
    h3=h3*28+c;
    h4=h4*44+c;
    if (h1==0xB16B88F1 && h2==0xFF5376F1 && h3==0x72AC5BF1 && h4==0x2F909AF1)
      return true;
  }
  return false;
}

// Decompress to stdout
static void decompress(FILE *in, FILE *out, long long int buf_len, int progress) {

	long long int len = 0;
	static int last_pct = 0, chunk = 0;
	int pct = 0;
  // Find start of block
  while (find_start(in)) {
    if (getc(in)!=LEVEL || getc(in)!=1)
      error("Not ZPAQ");

    // Read block header
    ZPAQL z;
    z.read(in);

    // PostProcessor and Decoder is created and and destroyed for each block
    PostProcessor pp(z);
    Decoder dec(in, z);

    // Read segments
    int c;
    while ((c=getc(in))==1) {
      while (getc(in)>0) ;  // skip filename
      while (getc(in)>0) ;  // skip comment
      if (getc(in)) error("reserved");  // reserved 0

      // Decompress
      SHA1 sha1;
      pp.set(out, &sha1);
     while ((c=dec.decompress())!=EOF) {
	if (progress) {
		len++;
		pct = (len * 100 / buf_len);
		if (pct != last_pct) {
			fprintf(stderr, "\r        ZPAQ Chunk %d of 2 Decompress: %i%%  \r", (chunk + 1), pct);
			fflush(stderr);
			last_pct = pct;
		}
	}
       pp.write(c);
      }
     pp.write(-1);

      // Check for end of segment and block markers
      int eos=getc(in);  // 253=SHA1 follows, 254=EOS
      if (eos==253) {
        for (int i=0; i<20; ++i) {
          if (getc(in)!=sha1.result(i))
            error("Checksum verify error");
        }
      }
      else if (eos!=254)
        error("missing end of segment marker");
    }
    if (c!=255) error("missing end of block marker");
  }
  if (progress) {
	fprintf(stderr, "\t                                             \r");
	fflush(stderr);
  }
  chunk ^= 1;
}

extern "C" void zpipe_decompress(FILE *in, FILE *out, long long int buf_len, int progress)
{
	decompress(in, out, buf_len, progress);
}

//////////////////////////// Compressor ////////////////////////////

//////////////////////////// Encoder ///////////////////////////////

// Encoder compresses using an arithmetic code
class Encoder {
  FILE* out;  // destination
  U32 low, high; // range
  Predictor pr;  // to get p
  void encode(int y, int p); // encode bit y (0..1) with probability p (0..8191)
public:
  Encoder(FILE* f, ZPAQL& z);
  void compress(int c);  // c is 0..255 or EOF
  void stat() {pr.stat();}  // print predictor statistics
  void setOutput(FILE* f) {out=f;}
};

// f = output file. z = compression model, already initialized
Encoder::Encoder(FILE* f, ZPAQL& z): 
  out(f), low(1), high(0xFFFFFFFF), pr(z) {}

// compress bit y with 16 bit probability p
inline void Encoder::encode(int y, int p) {
  assert(out);
  assert(p>=0 && p<65536);
  assert(y==0 || y==1);
  assert(high>low && low>0);
  U32 mid=low+((high-low)>>16)*p+((((high-low)&0xffff)*p)>>16); // split range
  assert(high>mid && mid>=low);
  if (y) high=mid; else low=mid+1; // pick half
  while ((high^low)<0x1000000) { // write identical leading bytes
    putc(high>>24, out);  // same as low>>24
    high=high<<8|255;
    low=low<<8;
    low+=(low==0); // so we don't code 4 0 bytes in a row
  }
}

// compress byte c
void Encoder::compress(int c) {
  assert(out);
  if (c==-1)
    encode(1, 0);
  else {
    assert(c>=0 && c<=255);
    encode(0, 0);
    for (int i=7; i>=0; --i) {
      int p=pr.predict()*2+1;
      assert(p>0 && p<65536);
      int y=c>>i&1;
      encode(y, p);
      pr.update(y);
    }
  }
}

//////////////////////////// Compress ////////////////////////////

// Compress to 1 segment in 1 block
static void compress(FILE *in, FILE *out, long long int buf_len, int progress) {

  // Compiled initialization lists generated by "zpaq vtrmid.cfg"
  static U8 header[71]={ // COMP 34 bytes from mid.cfg
    69,0,3,3,0,0,8,3,5,8,13,0,8,17,1,8,
    18,2,8,18,3,8,19,4,4,22,24,7,16,0,7,24,
    255,0,
    // HCOMP 37 bytes
    17,104,74,4,95,1,59,112,10,25,59,112,10,25,59,112,
    10,25,59,112,10,25,59,112,10,25,59,10,59,112,25,69,
    207,8,112,56,0};

	long long int len = 0;
	static int last_pct = 0, chunk = 0;
	int pct = 0;
    // Initialize
  ZPAQL z;  // model
  z.load(34, 37, header);  // initialize model
  Encoder enc(out, z);  // initialize arithmetic coder
  SHA1 sha1;  // initialize checksum computer

  // Optional: append locator tag to non ZPAQ data allowing block to be found
  //fprintf(out, "%s", "\x37\x6B\x53\x74\xA0\x31\x83\xD3\x8C\xB2\x28\xB0\xD3");

  // Write block header
  fprintf(out, "zPQ%c%c", LEVEL, 1);
  z.write(out);

  // Write segment header with empty filename and comment
  putc(1, out);  // start of segment
  putc(0, out);  // filename terminator
  putc(0, out);  // comment terminator
  putc(0, out);  // reserved

  // Compress PCOMP or POST 0
  enc.compress(0);  // PASS (no preprocessing)

  // Compress input and compute checksum
  int c;
  while ((c=getc(in))!=EOF) {
	if (progress) {
		len++;
		pct = (len * 100 / buf_len);
		if (pct != last_pct) {
			fprintf(stderr, "\r\tZPAQ Chunk %i of 2 compress: %i%%          \r", (chunk + 1), pct);
			fflush(stderr);
			last_pct = pct;
		}
	}
    enc.compress(c);
    sha1.put(c);
  }
  if (progress) {
	fprintf(stderr, "\t                                             \r");
	fflush(stderr);
  }
 enc.compress(-1);  // end of segment

  // Write segment checksum and trailer
  fprintf(out, "%c%c%c%c%c", 0, 0, 0, 0, 253);
  for (int j=0; j<20; ++j)
    putc(sha1.result(j), out);

  // Write end of block
  putc(255, out);  // block trailer

  // Optional: append a byte not 'z' to allow non-ZPAQ data to be appended
  //putc(0, out);
  chunk ^= 1;
}

extern "C" void zpipe_compress(FILE *in, FILE *out, long long int buf_len, int progress)
{
	compress(in, out, buf_len, progress);
}
