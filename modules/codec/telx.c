/*****************************************************************************
 * telx.c : Minimalistic Teletext subtitles decoder
 *****************************************************************************
 * Copyright (C) 2007 Vincent Penne
 * Some code converted from ProjectX java dvb decoder (c) 2001-2005 by dvb.matt
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
/*****************************************************************************
 * 
 * information on teletext format can be found here : 
 * http://pdc.ro.nu/teletext.html
 *
 * TODO :
 *
 *****************************************************************************/
#include <vlc/vlc.h>
#include <assert.h>
#include <stdint.h>

// this is an ugly test so that this source compile for both 0.8.6 and current trunk version
// considere this hack to be temporary, but I need it so that I can maintain my patch
// for both current svn and 0.8.6 in an easy way.
#ifdef VLC_ENOITEM
// svn trunk
#include "vlc_vout.h"
#else
// 0.8.6
#include <vlc/vout.h>
#include <vlc/decoder.h>
#include <vlc/sout.h>
/* #include "vlc_es.h" */
/* #include "vlc_block.h" */
/* #include "vlc_video.h" */
/* #include "vlc_spu.h" */
#endif

#include "vlc_bits.h"
#include "vlc_codec.h"


//#define TELX_DEBUG

#ifdef TELX_DEBUG
# define dbg( a ) msg_Dbg a 
#else
# define dbg( a )
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int telx_conf_cb ( vlc_object_t *,      /* variable's object */
			  char const *,            /* variable name */
			  vlc_value_t,                 /* old value */
			  vlc_value_t,                 /* new value */
			  void * );                /* callback data */

/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );
static subpicture_t *Decode( decoder_t *, block_t ** );


#define PAGE_TEXT N_("Teletext page")
#define PAGE_LONGTEXT N_("Set displayed teletext page for subtitles, 0 for all pages, 888 should be a standard value. Just leave it to zero if your stream has only one language for subtitles.")

#define IGNORE_SUB_FLAG_TEXT N_("Ignore subtitle flag")
#define IGNORE_SUB_FLAG_LONGTEXT N_("Ignore the subtitle flag, try this if your subtitles don't appear.")

vlc_module_begin();
#   define TELX_CFG_PREFIX "telx-"
    set_description( _("Teletext subtitles decoder") );
    set_capability( "decoder", 50 );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_SCODEC );
    set_callbacks( Open, Close );

    add_integer( "telx-page", 0, telx_conf_cb, PAGE_TEXT, PAGE_LONGTEXT,
                 VLC_FALSE );
    add_bool( "telx-ignore-subtitle-flag", 0, telx_conf_cb, IGNORE_SUB_FLAG_TEXT, IGNORE_SUB_FLAG_LONGTEXT, 
	      VLC_TRUE );

vlc_module_end();

/****************************************************************************
 * Local structures
 ****************************************************************************/

struct decoder_sys_t
{
  int         i_align;
  int         is_subtitle[9];
  char        lines[32][128];
  char        prev_text[512];
  mtime_t     prev_pts;
  int         page;
  int         erase[9];
  uint16_t *  active_national_set[9];
};

/****************************************************************************
 * Local data
 ****************************************************************************/

static int i_wanted_page = 0; // default 0 = all pages
static vlc_bool_t b_ignore_sub_flag = 0;

//
// my doc only mentions 13 national characters, but experiments show there 
// are more, in france for example I already found two more (0x9 and 0xb)
//
// convertion is in this order :
//
// 0x23 0x24 0x40 0x5b 0x5c 0x5d 0x5e 0x5f 0x60 0x7b 0x7c 0x7d 0x7e (these are the standard ones)
// 0x08 0x09 0x0a 0x0b 0x0c 0x0d (apparently a control character) 0x0e 0x0f
//

static uint16_t national_subsets[][20] = {
  { 0x00a3, 0x0024, 0x0040, 0x00ab, 0x00bd, 0x00bb, 0x005e, 0x0023, 
    0x002d, 0x00bc, 0x00a6, 0x00be, 0x00f7 }, // english ,000

  { 0x00e9, 0x00ef, 0x00e0, 0x00eb, 0x00ea, 0x00f9, 0x00ee, 0x0023,
    0x00e8, 0x00e2, 0x00f4, 0x00fb, 0x00e7, 0, 0x00eb, 0, 0x00ef }, // french  ,001

  { 0x0023, 0x00a4, 0x00c9, 0x00c4, 0x00d6, 0x00c5, 0x00dc, 0x005f, 
    0x00e9, 0x00e4, 0x00f6, 0x00e5, 0x00fc }, // swedish,finnish,hungarian ,010

  { 0x0023, 0x016f, 0x010d, 0x0165, 0x017e, 0x00fd, 0x00ed, 0x0159, 
    0x00e9, 0x00e1, 0x0115, 0x00fa, 0x0161 }, // czech,slovak  ,011

  { 0x0023, 0x0024, 0x00a7, 0x00c4, 0x00d6, 0x00dc, 0x005e, 0x005f, 
    0x00b0, 0x00e4, 0x00f6, 0x00fc, 0x00df }, // german ,100

  { 0x00e7, 0x0024, 0x00a1, 0x00e1, 0x00e9, 0x00ed, 0x00f3, 0x00fa, 
    0x00bf, 0x00fc, 0x00f1, 0x00e8, 0x00e0 }, // portuguese,spanish ,101

  { 0x00a3, 0x0024, 0x00e9, 0x00b0, 0x00e7, 0x00bb, 0x005e, 0x0023, 
    0x00f9, 0x00e0, 0x00f2, 0x00e8, 0x00ec }, // italian  ,110

  { 0x0023, 0x00a4, 0x0162, 0x00c2, 0x015e, 0x0102, 0x00ce, 0x0131, 
    0x0163, 0x00e2, 0x015f, 0x0103, 0x00ee }, // rumanian ,111

  { 0x0023, 0x0024, 0x0160, 0x0117, 0x0119, 0x017d, 0x010d, 0x016b, 
    0x0161, 0x0105, 0x0173, 0x017e, 0x012f }, // lettish,lithuanian ,1000

  { 0x0023, 0x0144, 0x0105, 0x005a, 0x015a, 0x0141, 0x0107, 0x00f3, 
    0x0119, 0x017c, 0x015b, 0x0142, 0x017a }, // polish,  1001

  { 0x0023, 0x00cb, 0x010c, 0x0106, 0x017d, 0x0110, 0x0160, 0x00eb, 
    0x010d, 0x0107, 0x017e, 0x0111, 0x0161 }, // serbian,croatian,slovenian, 1010

  { 0x0023, 0x00f5, 0x0160, 0x00c4, 0x00d6, 0x017e, 0x00dc, 0x00d5, 
    0x0161, 0x00e4, 0x00f6, 0x017e, 0x00fc }, // estonian  ,1011

  { 0x0054, 0x011f, 0x0130, 0x015e, 0x00d6, 0x00c7, 0x00dc, 0x011e, 
    0x0131, 0x015f, 0x00f6, 0x00e7, 0x00fc }, // turkish  ,1100
};


/*****************************************************************************
 * Open: probe the decoder and return score
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to chose.
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t *) p_this;
    decoder_sys_t *p_sys = NULL;
    vlc_value_t    val;
    int            i;

    if( p_dec->fmt_in.i_codec != VLC_FOURCC('t','e','l','x'))
    {
        return VLC_EGENERIC;
    }

    p_dec->pf_decode_sub = Decode;
    p_sys = p_dec->p_sys = malloc( sizeof(decoder_sys_t) );
    if (p_sys == NULL) {
      msg_Err( p_dec, "out of memory" );
      return VLC_ENOMEM;
    }

    memset( p_sys, 0, sizeof(decoder_sys_t) );

    p_sys->i_align = 0;
    for (i=0; i<9; i++)
      p_sys->active_national_set[i] = national_subsets[1];

    var_Create( p_dec, "telx-page", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
    var_Get( p_dec, "telx-page", &val );
    i_wanted_page = val.i_int;

    var_Create( p_dec, "telx-ignore-subtitle-flag", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    var_Get( p_dec, "telx-ignore-subtitle-flag", &val );
    b_ignore_sub_flag = val.b_bool;

    return VLC_SUCCESS;

/*  error: */
/*     if (p_sys) { */
/*       free(p_sys); */
/*       p_sys = NULL; */
/*     } */
/*     return VLC_EGENERIC; */
}

/*****************************************************************************
 * Config callback
 *****************************************************************************/
static int telx_conf_cb ( vlc_object_t * obj,      /* variable's object */
			  char const * name,       /* variable name */
			  vlc_value_t oldv,        /* old value */
			  vlc_value_t newv,        /* new value */
			  void * data)             /* callback data */
{
  if (!strcmp(name, "telx-page")) {
    i_wanted_page = newv.i_int;
    dbg((obj, "display teletext page changed to %d\n", i_wanted_page));
  } else if (!strcmp(name, "telx-ignore-subtitle-flag")) {
    b_ignore_sub_flag = newv.b_bool;
    dbg((obj, "ignore sub flag changed to %d\n", (int) b_ignore_sub_flag));
  }

  return 0;
}


/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*) p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    free( p_sys );
}

/**************************
 * change bits endianness *
 **************************/
static uint8_t bytereverse(int n)
{
  n = (((n >> 1) & 0x55) | ((n << 1) & 0xaa));
  n = (((n >> 2) & 0x33) | ((n << 2) & 0xcc));
  n = (((n >> 4) & 0x0f) | ((n << 4) & 0xf0));
  return n;
}

static int hamming_8_4(int a)
{
  switch (a) {
  case 0xA8: 
    return 0;
  case 0x0B: 
    return 1;
  case 0x26: 
    return 2;
  case 0x85: 
    return 3;
  case 0x92: 
    return 4;
  case 0x31: 
    return 5;
  case 0x1C: 
    return 6;
  case 0xBF: 
    return 7;
  case 0x40: 
    return 8;
  case 0xE3: 
    return 9;
  case 0xCE: 
    return 10;
  case 0x6D: 
    return 11;
  case 0x7A: 
    return 12;
  case 0xD9: 
    return 13;
  case 0xF4: 
    return 14;
  case 0x57: 
    return 15;
  default: 
    return -1;     // decoding error , not yet corrected
  }
}

// utc-2 --> utf-8
// this is not a general function, but it's enough for what we do here
static void to_utf8(char * res, uint16_t ch)
{
  if(ch>=0x80){
    if(ch>=0x800){
      res[0]=(ch>>12)|0xE0;
      res[1]=((ch>>6)&0x3F)|0x80;
      res[2]=(ch&0x3F)|0x80;
      res[3]=0;
    } else {
      res[0]=(ch>>6)|0xC0;
      res[1]=(ch&0x3F)|0x80;
      res[2]=0;
    }
  } else {
    res[0]=ch;
    res[1]=0;
  }
}

static void decode_string(char * res, int res_len, 
			  decoder_sys_t *p_sys, int magazine, 
			  uint8_t * packet, int len)
{
  char utf8[7];
  char * pt = res;
  int i;

  for (i=0; i<len; i++) {
    int in = bytereverse(packet[i])&0x7f;
    uint16_t out = 32;
    size_t l;
    switch (in) { // special national characters
    case 0x23:
      out = p_sys->active_national_set[magazine][0]; 
      break; 
    case 0x24:
      out = p_sys->active_national_set[magazine][1]; 
      break; 
    case 0x40:
      out = p_sys->active_national_set[magazine][2]; 
      break; 
    case 0x5b:
      out = p_sys->active_national_set[magazine][3]; 
      break; 
    case 0x5c:
      out = p_sys->active_national_set[magazine][4]; 
      break; 
    case 0x5d:
      out = p_sys->active_national_set[magazine][5]; 
      break; 
    case 0x5e:
      out = p_sys->active_national_set[magazine][6]; 
      break; 
    case 0x5f:
      out = p_sys->active_national_set[magazine][7]; 
      break; 
    case 0x60:
      out = p_sys->active_national_set[magazine][8]; 
      break; 
    case 0x7b:
      out = p_sys->active_national_set[magazine][9]; 
      break; 
    case 0x7c:
      out = p_sys->active_national_set[magazine][10]; 
      break; 
    case 0x7d:
      out = p_sys->active_national_set[magazine][11]; 
      break; 
    case 0x7e:
      out = p_sys->active_national_set[magazine][12]; 
      break; 

    // some special control characters (empirical)
    case 0x0d:
      // apparently this starts a sequence that ends with 0xb 0xb
      while (i+1<len && (bytereverse(packet[i+1])&0x7f) != 0x0b) i++;
      i += 2;
      break;
      //goto skip;

    default:
      // non documented national range 0x08 - 0x0f
      if (in >= 0x08 && in <= 0x0f) {
	out = p_sys->active_national_set[magazine][13 + in - 8];
	break;
      }

      // normal ascii
      if (in > 32 && in < 0x7f)
	out = in;
    }

    // handle undefined national characters
    if (out == 0) out = 32;

    // convert to utf-8
    to_utf8(utf8, out);
    l = strlen(utf8);
    if (pt + l < res + res_len - 1) {
      strcpy(pt, utf8);
      pt += l;
    }

   //skip: ;
  }
 //end:
  *pt++ = 0;
}  

/*****************************************************************************
 * Decode:
 *****************************************************************************/
static subpicture_t *Decode( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t       *p_block;
    subpicture_t  *p_spu = NULL;
    video_format_t fmt;
    int error = 1;
    //int erase = 0;
    int len, offset;
    int wanted_magazine = i_wanted_page/100;
    int wanted_page = 0x10*((i_wanted_page%100)/10) | (i_wanted_page%10);
    int update = 0;
    char text[512], * pt = text;
    char line[256];
    int i, total;

    if( pp_block == NULL || *pp_block == NULL ) return NULL;
    p_block = *pp_block;
    *pp_block = NULL;

    len = p_block->i_buffer;
    for (offset = 1; offset+46 <= len ; offset += 46) {
      uint8_t * packet = (uint8_t *) p_block->p_buffer+offset;
      
      //dbg((p_dec, "header %02x %02x %02x\n", packet[0], packet[1], packet[2]));
/*       if (packet[1] != 0x2C) { */
/* 	printf("wrong header\n"); */
/* 	//goto error; */
/* 	continue; */
/*       } */

      int mpag = (hamming_8_4(packet[4]))<<4 | hamming_8_4(packet[5]);
      int row, magazine;
      if (mpag < 0) { // decode error
	dbg((p_dec, "mpag hamming error\n"));
	continue;
      }

      row = 0xFF & bytereverse(mpag);
      magazine = (7 & row) == 0 ? 8 : (7 & row);
      row >>= 3;

      if (i_wanted_page && magazine != wanted_magazine) continue;

      if (row == 0) {
	// row 0 : flags and header line
	int flag = 0;
	int a;
	char * t = NULL;
	
	for (a = 0; a < 6; a++)
	  flag |= (0xF & bytereverse(hamming_8_4(packet[8+a]) )>>4 ) <<(a*4);

	dbg((p_dec, "mag %d flags %x page %x character set %d subtitles %d\n%s\n", magazine, flag, 
	     //p_sys->page, 
	     (0xF0 & bytereverse(hamming_8_4(packet[7]) )) |
	     (0xF & bytereverse(hamming_8_4(packet[6]) )>>4 ),
	     7 & flag>>21, 1 & flag>>15, t));

/* 	if (!b_ignore_sub_flag && !(1 & flag>>15)) */
/* 	  continue; */

	p_sys->page = (0xF0 & bytereverse(hamming_8_4(packet[7]) )) |
	  (0xF & bytereverse(hamming_8_4(packet[6]) )>>4 );

	decode_string(line, sizeof(line), p_sys, magazine, packet+14, 40-14);

	p_sys->active_national_set[magazine] = national_subsets[7 & flag>>21];

	p_sys->is_subtitle[magazine] = b_ignore_sub_flag || (1 & flag>>15);
	
 	if ((i_wanted_page && p_sys->page != wanted_page) || !p_sys->is_subtitle[magazine])
 	  continue;

	p_sys->erase[magazine] = (1 & flag>>7);

	dbg((p_dec, "%ld --> %ld\n", (long int) p_block->i_pts, (long int)(p_sys->prev_pts+1500000)));
	// kludge here : 
	// we ignore the erase flag if it happens less than  1.5 secondes before last caption
	// TODO make this time configurable
	if (p_block->i_pts > p_sys->prev_pts + 1500000 && 
	    p_sys->erase[magazine]) {
	  int i;
	  
	  dbg((p_dec, "ERASE !\n"));

	  p_sys->erase[magazine] = 0;	  
	  for (i=1; i<32; i++) {
	    if (!p_sys->lines[i][0]) continue;
	    //update = 1;
	    p_sys->lines[i][0] = 0;
	  }
	}

	// replace the row if it's different
	if (strcmp(line, p_sys->lines[row])) {
	  strncpy(p_sys->lines[row], line, sizeof(p_sys->lines[row])-1);
	}
	update = 1;

      } else if (row < 24) {
	char * t;
	int i;
	// row 1-23 : normal lines

 	if ((i_wanted_page && p_sys->page != wanted_page) || !p_sys->is_subtitle[magazine])
 	  continue;

	decode_string(line, sizeof(line), p_sys, magazine, packet+6, 40);
	t = line;
	// remove starting spaces
	while (*t == 32) t++;
	// remove trailing spaces
	for (i=strlen(t)-1; i>=0 && t[i] == 32; i--);
	t[i+1] = 0;

	// replace the row if it's different
	if (strcmp(t, p_sys->lines[row])) {
	  strncpy(p_sys->lines[row], t, sizeof(p_sys->lines[row])-1);
	  update = 1;
	}

	if (t[0])
	  p_sys->prev_pts = p_block->i_pts;

	dbg((p_dec, "%d %d : ", magazine, row));
	dbg((p_dec, "%s\n", t));

#ifdef TELX_DEBUG
	{
	  char dbg[256];
	  dbg[0] = 0;
	  for (i=0; i<40; i++) {
	    int in = bytereverse(packet[6+i]) & 0x7f;
	    sprintf(dbg+strlen(dbg), "%02x ", in);
	  }
	  dbg((p_dec, "%s\n", dbg));
	  dbg[0] = 0;
	  for (i=0; i<40; i++) {
	    decode_string(line, sizeof(line), p_sys, magazine, packet+6+i, 1);
	    sprintf(dbg+strlen(dbg), "%s  ", line);
	  }
	  dbg((p_dec, "%s\n", dbg));
	}
#endif
	
      } else if (row == 25) {
	// row 25 : alternate header line
 	if ((i_wanted_page && p_sys->page != wanted_page) || !p_sys->is_subtitle[magazine])
 	  continue;

	decode_string(line, sizeof(line), p_sys, magazine, packet+6, 40);
	// replace the row if it's different
	if (strcmp(line, p_sys->lines[0])) {
	  strncpy(p_sys->lines[0], line, sizeof(p_sys->lines[0])-1);
	  //update = 1;
	}
      }
/*       else if (row == 26) { */
/* 	// row 26 : TV listings */
/*       } else */
/* 	dbg((p_dec, "%d %d : %s\n", magazine, row, decode_string(p_sys, magazine, packet+6, 40))); */
    }

    if (!update)
      goto error;

    total = 0;
    for (i=1; i<24; i++) {
      size_t l = strlen(p_sys->lines[i]);
      if (l > sizeof(text)-total-1)
	l = sizeof(text)-total-1;
      if (l > 0) {
	memcpy(pt, p_sys->lines[i], l);
	total += l;
	pt += l;
	if (sizeof(text)-total-1 > 0) {
	  *pt++ = '\n';
	  total++;
	}
      }
    }
    *pt = 0;

    if (!strcmp(text, p_sys->prev_text))
      goto error;

    dbg((p_dec, "UPDATE TELETEXT PICTURE\n"));

    assert(sizeof(p_sys->prev_text)>=sizeof(text));
    strcpy(p_sys->prev_text, text);

    /* Create the subpicture unit */
    p_spu = p_dec->pf_spu_buffer_new( p_dec );
    if( !p_spu ) {
      msg_Warn( p_dec, "can't get spu buffer" );
      goto error;
    }
    
    p_spu->b_pausable = VLC_TRUE;
    
    /* Create a new subpicture region */
    memset( &fmt, 0, sizeof(video_format_t) );
    fmt.i_chroma = VLC_FOURCC('T','E','X','T');
    fmt.i_aspect = 0;
    fmt.i_width = fmt.i_height = 0;
    fmt.i_x_offset = fmt.i_y_offset = 0;
    p_spu->p_region = p_spu->pf_create_region( VLC_OBJECT(p_dec), &fmt );
    if( !p_spu->p_region ) {
      msg_Err( p_dec, "cannot allocate SPU region" );
      goto error;
    }

    /* Normal text subs, easy markup */
    p_spu->i_flags = SUBPICTURE_ALIGN_BOTTOM | p_sys->i_align;
    p_spu->i_x = p_sys->i_align ? 20 : 0;
    p_spu->i_y = 10;

    p_spu->p_region->psz_text = strdup(text);
    p_spu->i_start = p_block->i_pts;
    p_spu->i_stop = p_block->i_pts + p_block->i_length;
    p_spu->b_ephemer = (p_block->i_length == 0);
    p_spu->b_absolute = VLC_FALSE;
    dbg((p_dec, "%ld --> %ld\n", (long int) p_block->i_pts/100000, (long int)p_block->i_length/100000));

    error = 0;

error:
    if (error && p_spu) {
      p_dec->pf_spu_buffer_del( p_dec, p_spu );
      p_spu = NULL;
    }

    block_Release( p_block );

    return p_spu;
}

