/*****************************************************************************
 * objects.c: vlc_object_t handling
 *****************************************************************************
 * Copyright (C) 2002 VideoLAN
 * $Id: objects.c,v 1.26 2002/10/17 13:15:31 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>

#ifdef HAVE_STDLIB_H
#   include <stdlib.h>                                          /* realloc() */
#endif

#include "stream_control.h"
#include "input_ext-intf.h"
#include "input_ext-dec.h"

#include "video.h"
#include "video_output.h"

#include "audio_output.h"
#include "aout_internal.h"

#include "vlc_playlist.h"
#include "interface.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  DumpCommand( vlc_object_t *, char *, char * );

static vlc_object_t * FindObject    ( vlc_object_t *, int, int );
static void           DetachObject  ( vlc_object_t * );
static void           PrintObject   ( vlc_object_t *, const char * );
static void           DumpStructure ( vlc_object_t *, int, char * );
static int            FindIndex     ( vlc_object_t *, vlc_object_t **, int );
static void           SetAttachment ( vlc_object_t *, vlc_bool_t );

static vlc_list_t *   NewList       ( void );
static vlc_list_t *   ListAppend    ( vlc_list_t *, vlc_object_t * );

/*****************************************************************************
 * Local structure lock
 *****************************************************************************/
static vlc_mutex_t    structure_lock;

/*****************************************************************************
 * vlc_object_create: initialize a vlc object
 *****************************************************************************
 * This function allocates memory for a vlc object and initializes it. If
 * i_type is not a known value such as VLC_OBJECT_ROOT, VLC_OBJECT_VOUT and
 * so on, vlc_object_create will use its value for the object size.
 *****************************************************************************/
void * __vlc_object_create( vlc_object_t *p_this, int i_type )
{
    vlc_object_t * p_new;
    char *         psz_type;
    size_t         i_size;

    switch( i_type )
    {
        case VLC_OBJECT_ROOT:
            i_size = sizeof(libvlc_t);
            psz_type = "root";
            break;
        case VLC_OBJECT_VLC:
            i_size = sizeof(vlc_t);
            psz_type = "vlc";
            break;
        case VLC_OBJECT_MODULE:
            i_size = sizeof(module_t);
            psz_type = "module";
            break;
        case VLC_OBJECT_INTF:
            i_size = sizeof(intf_thread_t);
            psz_type = "interface";
            break;
        case VLC_OBJECT_PLAYLIST:
            i_size = sizeof(playlist_t);
            psz_type = "playlist";
            break;
        case VLC_OBJECT_INPUT:
            i_size = sizeof(input_thread_t);
            psz_type = "input";
            break;
        case VLC_OBJECT_DECODER:
            i_size = sizeof(decoder_fifo_t);
            psz_type = "decoder";
            break;
        case VLC_OBJECT_VOUT:
            i_size = sizeof(vout_thread_t);
            psz_type = "video output";
            break;
        case VLC_OBJECT_AOUT:
            i_size = sizeof(aout_instance_t);
            psz_type = "audio output";
            break;
        default:
            i_size = i_type > 0
                      ? i_type > sizeof(vlc_object_t)
                         ? i_type : sizeof(vlc_object_t)
                      : sizeof(vlc_object_t);
            i_type = VLC_OBJECT_GENERIC;
            psz_type = "generic";
            break;
    }

    if( i_type == VLC_OBJECT_ROOT )
    {
        p_new = p_this;
    }
    else
    {
        p_new = malloc( i_size );

        if( !p_new )
        {
            return NULL;
        }

        memset( p_new, 0, i_size );
    }

    p_new->i_object_type = i_type;
    p_new->psz_object_type = psz_type;

    p_new->psz_object_name = NULL;

    p_new->b_die = VLC_FALSE;
    p_new->b_error = VLC_FALSE;
    p_new->b_dead = VLC_FALSE;
    p_new->b_attached = VLC_FALSE;

    p_new->i_vars = 0;
    p_new->p_vars = (variable_t *)malloc( 16 * sizeof( variable_t ) );

    if( !p_new->p_vars )
    {
        free( p_new );
        return NULL;
    }

    if( i_type == VLC_OBJECT_ROOT )
    {
        /* If i_type is root, then p_new is actually p_libvlc */
        p_new->p_libvlc = (libvlc_t*)p_new;
        p_new->p_vlc = NULL;

        p_new->p_libvlc->i_counter = 0;
        p_new->i_object_id = 0;

        p_new->p_libvlc->i_objects = 1;
        p_new->p_libvlc->pp_objects = malloc( sizeof(vlc_object_t *) );
        p_new->p_libvlc->pp_objects[0] = p_new;
        p_new->b_attached = VLC_TRUE;
    }
    else
    {
        p_new->p_libvlc = p_this->p_libvlc;
        p_new->p_vlc = ( i_type == VLC_OBJECT_VLC ) ? (vlc_t*)p_new
                                                    : p_this->p_vlc;

        vlc_mutex_lock( &structure_lock );

        p_new->p_libvlc->i_counter++;
        p_new->i_object_id = p_new->p_libvlc->i_counter;

        /* Wooohaa! If *this* fails, we're in serious trouble! Anyway it's
         * useless to try and recover anything if pp_objects gets smashed. */
        p_new->p_libvlc->i_objects++;
        p_new->p_libvlc->pp_objects =
                realloc( p_new->p_libvlc->pp_objects,
                         p_new->p_libvlc->i_objects * sizeof(vlc_object_t *) );
        p_new->p_libvlc->pp_objects[ p_new->p_libvlc->i_objects - 1 ] = p_new;

        vlc_mutex_unlock( &structure_lock );
    }

    p_new->i_refcount = 0;
    p_new->p_parent = NULL;
    p_new->pp_children = NULL;
    p_new->i_children = 0;

    p_new->p_private = NULL;

    /* Initialize mutexes and condvars */
    vlc_mutex_init( p_new, &p_new->object_lock );
    vlc_cond_init( p_new, &p_new->object_wait );
    vlc_mutex_init( p_new, &p_new->var_lock );

    if( i_type == VLC_OBJECT_ROOT )
    {
        vlc_mutex_init( p_new, &structure_lock );

        var_Create( p_new, "list", VLC_VAR_COMMAND );
        var_Set( p_new, "list", (vlc_value_t)(void*)DumpCommand );
        var_Create( p_new, "tree", VLC_VAR_COMMAND );
        var_Set( p_new, "tree", (vlc_value_t)(void*)DumpCommand );
    }

    return p_new;
}

/*****************************************************************************
 * vlc_object_destroy: destroy a vlc object
 *****************************************************************************
 * This function destroys an object that has been previously allocated with
 * vlc_object_create. The object's refcount must be zero and it must not be
 * attached to other objects in any way.
 *****************************************************************************/
void __vlc_object_destroy( vlc_object_t *p_this )
{
    int i_delay = 0;

    if( p_this->i_children )
    {
        msg_Err( p_this, "cannot delete object with children" );
        return;
    }

    if( p_this->p_parent )
    {
        msg_Err( p_this, "cannot delete object with a parent" );
        return;
    }

    while( p_this->i_refcount )
    {
        i_delay++;

        /* Don't warn immediately ... 100ms seems OK */
        if( i_delay == 2 )
        {
            msg_Warn( p_this, "refcount is %i, delaying before deletion",
                              p_this->i_refcount );
        }
        else if( i_delay == 12 )
        {
            msg_Err( p_this, "refcount is %i, I have a bad feeling about this",
                             p_this->i_refcount );
        }
        else if( i_delay == 42 )
        {
            msg_Err( p_this, "we waited too long, cancelling destruction" );
            return;
        }

        msleep( 100000 );
    }

    /* Destroy the associated variables, starting from the end so that
     * no memmove calls have to be done. */
    while( p_this->i_vars )
    {
        var_Destroy( p_this, p_this->p_vars[p_this->i_vars - 1].psz_name );
    }

    free( p_this->p_vars );
    vlc_mutex_destroy( &p_this->var_lock );

    if( p_this->i_object_type == VLC_OBJECT_ROOT )
    {
        /* We are the root object ... no need to lock. */
        free( p_this->p_libvlc->pp_objects );
        p_this->p_libvlc->pp_objects = NULL;

        vlc_mutex_destroy( &structure_lock );
    }
    else
    {
        int i_index;

        vlc_mutex_lock( &structure_lock );

        /* Wooohaa! If *this* fails, we're in serious trouble! Anyway it's
         * useless to try and recover anything if pp_objects gets smashed. */
        i_index = FindIndex( p_this, p_this->p_libvlc->pp_objects,
                             p_this->p_libvlc->i_objects );
        memmove( p_this->p_libvlc->pp_objects + i_index,
                 p_this->p_libvlc->pp_objects + i_index + 1,
                 (p_this->p_libvlc->i_objects - i_index - 1)
                   * sizeof( vlc_object_t *) );

        p_this->p_libvlc->pp_objects =
            realloc( p_this->p_libvlc->pp_objects,
                 (p_this->p_libvlc->i_objects - 1) * sizeof(vlc_object_t *) );

        vlc_mutex_unlock( &structure_lock );
    }

    p_this->p_libvlc->i_objects--;

    vlc_mutex_destroy( &p_this->object_lock );
    vlc_cond_destroy( &p_this->object_wait );

    free( p_this );
}

/*****************************************************************************
 * vlc_object_get: find an object given its ID
 *****************************************************************************
 * This function looks for the object whose i_object_id field is i_id. We
 * use a dichotomy so that lookups are in log2(n).
 *****************************************************************************/
void * __vlc_object_get( vlc_object_t *p_this, int i_id )
{
    int i_max, i_middle;
    vlc_object_t **pp_objects;

    vlc_mutex_lock( &structure_lock );

    pp_objects = p_this->p_libvlc->pp_objects;

    /* Perform our dichotomy */
    for( i_max = p_this->p_libvlc->i_objects - 1 ; ; )
    {
        i_middle = i_max / 2;

        if( pp_objects[i_middle]->i_object_id > i_id )
        {
            i_max = i_middle;
        }
        else if( pp_objects[i_middle]->i_object_id < i_id )
        {
            if( i_middle )
            {
                pp_objects += i_middle;
                i_max -= i_middle;
            }
            else
            {
                /* This happens when there are only two remaining objects */
                if( pp_objects[i_middle+1]->i_object_id == i_id )
                {
                    vlc_mutex_unlock( &structure_lock );
                    return pp_objects[i_middle+1];
                }
                break;
            }
        }
        else
        {
            vlc_mutex_unlock( &structure_lock );
            return pp_objects[i_middle];
        }

        if( i_max == 0 )
        {
            /* this means that i_max == i_middle, and since we have already
             * tested pp_objects[i_middle]), p_found is properly set. */
            break;
        }
    }

    vlc_mutex_unlock( &structure_lock );
    return NULL;
}

/*****************************************************************************
 * vlc_object_find: find a typed object and increment its refcount
 *****************************************************************************
 * This function recursively looks for a given object type. i_mode can be one
 * of FIND_PARENT, FIND_CHILD or FIND_ANYWHERE.
 *****************************************************************************/
void * __vlc_object_find( vlc_object_t *p_this, int i_type, int i_mode )
{
    vlc_object_t *p_found;

    vlc_mutex_lock( &structure_lock );

    /* If we are of the requested type ourselves, don't look further */
    if( !(i_mode & FIND_STRICT) && p_this->i_object_type == i_type )
    {
        p_this->i_refcount++;
        vlc_mutex_unlock( &structure_lock );
        return p_this;
    }

    /* Otherwise, recursively look for the object */
    if( (i_mode & 0x000f) == FIND_ANYWHERE )
    {
        p_found = FindObject( VLC_OBJECT(p_this->p_vlc), i_type,
                              (i_mode & ~0x000f) | FIND_CHILD );
    }
    else
    {
        p_found = FindObject( p_this, i_type, i_mode );
    }

    vlc_mutex_unlock( &structure_lock );

    return p_found;
}

/*****************************************************************************
 * vlc_object_yield: increment an object refcount
 *****************************************************************************/
void __vlc_object_yield( vlc_object_t *p_this )
{
    vlc_mutex_lock( &structure_lock );
    p_this->i_refcount++;
    vlc_mutex_unlock( &structure_lock );
}

/*****************************************************************************
 * vlc_object_release: decrement an object refcount
 *****************************************************************************/
void __vlc_object_release( vlc_object_t *p_this )
{
    vlc_mutex_lock( &structure_lock );
    p_this->i_refcount--;
    vlc_mutex_unlock( &structure_lock );
}

/*****************************************************************************
 * vlc_object_attach: attach object to a parent object
 *****************************************************************************
 * This function sets p_this as a child of p_parent, and p_parent as a parent
 * of p_this. This link can be undone using vlc_object_detach.
 *****************************************************************************/
void __vlc_object_attach( vlc_object_t *p_this, vlc_object_t *p_parent )
{
    vlc_mutex_lock( &structure_lock );

    /* Attach the parent to its child */
    p_this->p_parent = p_parent;

    /* Attach the child to its parent */
    p_parent->i_children++;
    p_parent->pp_children = (vlc_object_t **)realloc( p_parent->pp_children,
                               p_parent->i_children * sizeof(vlc_object_t *) );
    p_parent->pp_children[p_parent->i_children - 1] = p_this;

    /* Climb up the tree to see whether we are connected with the root */
    if( p_parent->b_attached )
    {
        SetAttachment( p_this, VLC_TRUE );
    }

    vlc_mutex_unlock( &structure_lock );
}

/*****************************************************************************
 * vlc_object_detach: detach object from its parent
 *****************************************************************************
 * This function removes all links between an object and its parent.
 *****************************************************************************/
void __vlc_object_detach( vlc_object_t *p_this )
{
    vlc_mutex_lock( &structure_lock );
    if( !p_this->p_parent )
    {
        msg_Err( p_this, "object is not attached" );
        vlc_mutex_unlock( &structure_lock );
        return;
    }

    /* Climb up the tree to see whether we are connected with the root */
    if( p_this->p_parent->b_attached )
    {
        SetAttachment( p_this, VLC_FALSE );
    }

    DetachObject( p_this );
    vlc_mutex_unlock( &structure_lock );
}

/*****************************************************************************
 * vlc_list_find: find a list typed objects and increment their refcount
 *****************************************************************************
 * This function recursively looks for a given object type. i_mode can be one
 * of FIND_PARENT, FIND_CHILD or FIND_ANYWHERE.
 *****************************************************************************/
vlc_list_t * __vlc_list_find( vlc_object_t *p_this, int i_type, int i_mode )
{
    vlc_list_t *p_list = NewList();

    vlc_mutex_lock( &structure_lock );

    /* Look for the objects */
    if( (i_mode & 0x000f) == FIND_ANYWHERE )
    {
        vlc_object_t **pp_current, **pp_end;

        pp_current = p_this->p_libvlc->pp_objects;
        pp_end = pp_current + p_this->p_libvlc->i_objects;

        for( ; pp_current < pp_end ; pp_current++ )
        {
            if( (*pp_current)->b_attached
                 && (*pp_current)->i_object_type == i_type )
            {
                p_list = ListAppend( p_list, *pp_current );
            }
        }
    }
    else
    {
        msg_Err( p_this, "unimplemented!" );
    }

    vlc_mutex_unlock( &structure_lock );

    return p_list;
}

/*****************************************************************************
 * DumpCommand: print the current vlc structure
 *****************************************************************************
 * This function prints either an ASCII tree showing the connections between
 * vlc objects, and additional information such as their refcount, thread ID,
 * etc. (command "tree"), or the same data as a simple list (command "list").
 *****************************************************************************/
static int DumpCommand( vlc_object_t *p_this, char *psz_cmd, char *psz_arg )
{
    if( *psz_cmd == 't' )
    {
        char psz_foo[2 * MAX_DUMPSTRUCTURE_DEPTH + 1];
        vlc_object_t *p_object;

        if( *psz_arg )
        {
            p_object = vlc_object_get( p_this, atoi(psz_arg) );

            if( !p_object )
            {
                return VLC_ENOOBJ;
            }
        }
        else
        {
            p_object = p_this->p_vlc ? VLC_OBJECT(p_this->p_vlc) : p_this;
        }

        vlc_mutex_lock( &structure_lock );

        psz_foo[0] = '|';
        DumpStructure( p_object, 0, psz_foo );

        vlc_mutex_unlock( &structure_lock );
    }
    else if( *psz_cmd == 'l' )
    {
        vlc_object_t **pp_current, **pp_end;

        vlc_mutex_lock( &structure_lock );

        pp_current = p_this->p_libvlc->pp_objects;
        pp_end = pp_current + p_this->p_libvlc->i_objects;

        for( ; pp_current < pp_end ; pp_current++ )
        {
            if( (*pp_current)->b_attached )
            {
                PrintObject( *pp_current, "" );
            }
            else
            {
                printf( " o %.8i %s (not attached)\n",
                        (*pp_current)->i_object_id,
                        (*pp_current)->psz_object_type );
            }
        }

        vlc_mutex_unlock( &structure_lock );
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * vlc_list_release: free a list previously allocated by vlc_list_find
 *****************************************************************************
 * This function decreases the refcount of all objects in the list and
 * frees the list.
 *****************************************************************************/
void vlc_list_release( vlc_list_t *p_list )
{
    if( p_list->i_count )
    {
        vlc_object_t ** pp_current = p_list->pp_objects;

        vlc_mutex_lock( &structure_lock );

        while( pp_current[0] )
        {
            pp_current[0]->i_refcount--;
            pp_current++;
        }

        vlc_mutex_unlock( &structure_lock );
    }

    free( p_list );
}

/* Following functions are local */

/*****************************************************************************
 * FindIndex: find the index of an object in an array of objects
 *****************************************************************************
 * This function assumes that p_this can be found in pp_objects. It will not
 * crash if p_this cannot be found, but will return a wrong value. It is your
 * duty to check the return value if you are not certain that the object could
 * be found for sure.
 *****************************************************************************/
static int FindIndex( vlc_object_t *p_this,
                      vlc_object_t **pp_objects, int i_count )
{
    int i_middle = i_count / 2;

    if( i_count == 0 )
    {
        return 0;
    }

    if( pp_objects[i_middle] == p_this )
    {
        return i_middle;
    }

    if( i_count == 1 )
    {
        return 0;
    }

    /* We take advantage of the sorted array */
    if( pp_objects[i_middle]->i_object_id < p_this->i_object_id )
    {
        return i_middle + FindIndex( p_this, pp_objects + i_middle,
                                             i_count - i_middle );
    }
    else
    {
        return FindIndex( p_this, pp_objects, i_middle );
    }
}

static vlc_object_t * FindObject( vlc_object_t *p_this, int i_type, int i_mode )
{
    int i;
    vlc_object_t *p_tmp;

    switch( i_mode & 0x000f )
    {
    case FIND_PARENT:
        p_tmp = p_this->p_parent;
        if( p_tmp )
        {
            if( p_tmp->i_object_type == i_type )
            {
                p_tmp->i_refcount++;
                return p_tmp;
            }
            else
            {
                return FindObject( p_tmp, i_type, i_mode );
            }
        }
        break;

    case FIND_CHILD:
        for( i = p_this->i_children; i--; )
        {
            p_tmp = p_this->pp_children[i];
            if( p_tmp->i_object_type == i_type )
            {
                p_tmp->i_refcount++;
                return p_tmp;
            }
            else if( p_tmp->i_children )
            {
                p_tmp = FindObject( p_tmp, i_type, i_mode );
                if( p_tmp )
                {
                    return p_tmp;
                }
            }
        }
        break;

    case FIND_ANYWHERE:
        /* Handled in vlc_object_find */
        break;
    }

    return NULL;
}

static void DetachObject( vlc_object_t *p_this )
{
    vlc_object_t *p_parent = p_this->p_parent;
    int i_index, i;

    /* Remove p_this's parent */
    p_this->p_parent = NULL;

    /* Remove all of p_parent's children which are p_this */
    for( i_index = p_parent->i_children ; i_index-- ; )
    {
        if( p_parent->pp_children[i_index] == p_this )
        {
            p_parent->i_children--;
            for( i = i_index ; i < p_parent->i_children ; i++ )
            {
                p_parent->pp_children[i] = p_parent->pp_children[i+1];
            }
        }
    }

    if( p_parent->i_children )
    {
        p_parent->pp_children = (vlc_object_t **)realloc( p_parent->pp_children,
                               p_parent->i_children * sizeof(vlc_object_t *) );
    }
    else
    {
        free( p_parent->pp_children );
        p_parent->pp_children = NULL;
    }
}

/*****************************************************************************
 * SetAttachment: recursively set the b_attached flag of a subtree.
 *****************************************************************************
 * This function is used by the attach and detach functions to propagate
 * the b_attached flag in a subtree.
 *****************************************************************************/
static void SetAttachment( vlc_object_t *p_this, vlc_bool_t b_attached )
{
    int i_index;

    for( i_index = p_this->i_children ; i_index-- ; )
    {
        SetAttachment( p_this->pp_children[i_index], b_attached );
    }

    p_this->b_attached = b_attached;
}

static void PrintObject( vlc_object_t *p_this, const char *psz_prefix )
{
    char psz_children[20], psz_refcount[20], psz_thread[20], psz_name[50];

    psz_name[0] = '\0';
    if( p_this->psz_object_name )
    {
        snprintf( psz_name, 50, " \"%s\"", p_this->psz_object_name );
        psz_name[48] = '\"';
        psz_name[49] = '\0';
    }

    psz_children[0] = '\0';
    switch( p_this->i_children )
    {
        case 0:
            break;
        case 1:
            strcpy( psz_children, ", 1 child" );
            break;
        default:
            snprintf( psz_children, 20,
                      ", %i children", p_this->i_children );
            psz_children[19] = '\0';
            break;
    }

    psz_refcount[0] = '\0';
    if( p_this->i_refcount )
    {
        snprintf( psz_refcount, 20, ", refcount %i", p_this->i_refcount );
        psz_refcount[19] = '\0';
    }

    psz_thread[0] = '\0';
    if( p_this->b_thread )
    {
        snprintf( psz_thread, 20, " (thread %d)", (int)p_this->thread_id );
        psz_thread[19] = '\0';
    }

    printf( " %so %.8i %s%s%s%s%s\n", psz_prefix,
            p_this->i_object_id, p_this->psz_object_type,
            psz_name, psz_thread, psz_refcount, psz_children );
}

static void DumpStructure( vlc_object_t *p_this, int i_level, char *psz_foo )
{
    int i;
    char i_back = psz_foo[i_level];
    psz_foo[i_level] = '\0';

    PrintObject( p_this, psz_foo );

    psz_foo[i_level] = i_back;

    if( i_level / 2 >= MAX_DUMPSTRUCTURE_DEPTH )
    {
        msg_Warn( p_this, "structure tree is too deep" );
        return;
    }

    for( i = 0 ; i < p_this->i_children ; i++ )
    {
        if( i_level )
        {
            psz_foo[i_level-1] = ' ';

            if( psz_foo[i_level-2] == '`' )
            {
                psz_foo[i_level-2] = ' ';
            }
        }

        if( i == p_this->i_children - 1 )
        {
            psz_foo[i_level] = '`';
        }
        else
        {
            psz_foo[i_level] = '|';
        }

        psz_foo[i_level+1] = '-';
        psz_foo[i_level+2] = '\0';

        DumpStructure( p_this->pp_children[i], i_level + 2, psz_foo );
    }
}

static vlc_list_t * NewList( void )
{
    vlc_list_t *p_list = malloc( sizeof( vlc_list_t )
                                     + 3 * sizeof( vlc_object_t * ) );

    if( p_list == NULL )
    {
        return NULL;
    }

    p_list->i_count = 0;
    p_list->pp_objects = &p_list->_p_first;

    /* We allocated space for NULL and for three extra objects */
    p_list->_i_extra = 3;
    p_list->_p_first = NULL;

    return p_list;
}

static vlc_list_t * ListAppend( vlc_list_t *p_list, vlc_object_t *p_object )
{
    if( p_list == NULL )
    {
        return NULL;
    }

    if( p_list->_i_extra == 0 )
    {
        /* If we had X objects it means the array has a size of X+1, we
         * make it size 2X+2, so we alloc 2X+1 because there is already
         * one allocated in the real structure */
        p_list = realloc( p_list, sizeof( vlc_list_t )
                                   + (p_list->i_count * 2 + 1)
                                       * sizeof( vlc_object_t * ) );
        if( p_list == NULL ) 
        {
            return NULL;
        }

        /* We have X+1 extra slots */
        p_list->_i_extra = p_list->i_count + 1;
        p_list->pp_objects = &p_list->_p_first;
    }

    p_object->i_refcount++;

    p_list->pp_objects[p_list->i_count] = p_object;
    p_list->i_count++;
    p_list->pp_objects[p_list->i_count] = NULL;
    p_list->_i_extra--;

    return p_list;
}

