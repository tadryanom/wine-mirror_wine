/*
 * Window related functions
 *
 * Copyright 1993, 1994 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#if 0
#pragma makedep unix
#endif

#include <pthread.h>
#include <assert.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/server.h"

#define NB_USER_HANDLES  ((LAST_USER_HANDLE - FIRST_USER_HANDLE + 1) >> 1)
#define USER_HANDLE_TO_INDEX(hwnd) ((LOWORD(hwnd) - FIRST_USER_HANDLE) >> 1)

static void *user_handles[NB_USER_HANDLES];

static struct list window_surfaces = LIST_INIT( window_surfaces );
static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

/***********************************************************************
 *           alloc_user_handle
 */
HANDLE alloc_user_handle( struct user_object *ptr, unsigned int type )
{
    HANDLE handle = 0;

    SERVER_START_REQ( alloc_user_handle )
    {
        if (!wine_server_call_err( req )) handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    if (handle)
    {
        UINT index = USER_HANDLE_TO_INDEX( handle );

        assert( index < NB_USER_HANDLES );
        ptr->handle = handle;
        ptr->type = type;
        InterlockedExchangePointer( &user_handles[index], ptr );
    }
    return handle;
}

/***********************************************************************
 *           get_user_handle_ptr
 */
void *get_user_handle_ptr( HANDLE handle, unsigned int type )
{
    struct user_object *ptr;
    WORD index = USER_HANDLE_TO_INDEX( handle );

    if (index >= NB_USER_HANDLES) return NULL;

    user_lock();
    if ((ptr = user_handles[index]))
    {
        if (ptr->type == type &&
            ((UINT)(UINT_PTR)ptr->handle == (UINT)(UINT_PTR)handle ||
             !HIWORD(handle) || HIWORD(handle) == 0xffff))
            return ptr;
        ptr = NULL;
    }
    else ptr = OBJ_OTHER_PROCESS;
    user_unlock();
    return ptr;
}

/***********************************************************************
 *           set_user_handle_ptr
 */
void set_user_handle_ptr( HANDLE handle, struct user_object *ptr )
{
    WORD index = USER_HANDLE_TO_INDEX(handle);
    assert( index < NB_USER_HANDLES );
    InterlockedExchangePointer( &user_handles[index], ptr );
}

/***********************************************************************
 *           release_user_handle_ptr
 */
void release_user_handle_ptr( void *ptr )
{
    assert( ptr && ptr != OBJ_OTHER_PROCESS );
    user_unlock();
}

/***********************************************************************
 *           free_user_handle
 */
void *free_user_handle( HANDLE handle, unsigned int type )
{
    struct user_object *ptr;
    WORD index = USER_HANDLE_TO_INDEX( handle );

    if ((ptr = get_user_handle_ptr( handle, type )) && ptr != OBJ_OTHER_PROCESS)
    {
        SERVER_START_REQ( free_user_handle )
        {
            req->handle = wine_server_user_handle( handle );
            if (wine_server_call( req )) ptr = NULL;
            else InterlockedCompareExchangePointer( &user_handles[index], NULL, ptr );
        }
        SERVER_END_REQ;
        user_unlock();
    }
    return ptr;
}

/***********************************************************************
 *           next_thread_window
 */
WND *next_thread_window_ptr( HWND *hwnd )
{
    struct user_object *ptr;
    WND *win;
    WORD index = *hwnd ? USER_HANDLE_TO_INDEX( *hwnd ) + 1 : 0;

    while (index < NB_USER_HANDLES)
    {
        if (!(ptr = user_handles[index++])) continue;
        if (ptr->type != NTUSER_OBJ_WINDOW) continue;
        win = (WND *)ptr;
        if (win->tid != GetCurrentThreadId()) continue;
        *hwnd = ptr->handle;
        return win;
    }
    return NULL;
}

/*******************************************************************
 *           register_window_surface
 *
 * Register a window surface in the global list, possibly replacing another one.
 */
void register_window_surface( struct window_surface *old, struct window_surface *new )
{
    if (old == new) return;
    pthread_mutex_lock( &surfaces_lock );
    if (old) list_remove( &old->entry );
    if (new) list_add_tail( &window_surfaces, &new->entry );
    pthread_mutex_unlock( &surfaces_lock );
}

/*******************************************************************
 *           flush_window_surfaces
 *
 * Flush pending output from all window surfaces.
 */
void flush_window_surfaces( BOOL idle )
{
    static DWORD last_idle;
    DWORD now;
    struct window_surface *surface;

    pthread_mutex_lock( &surfaces_lock );
    now = NtGetTickCount();
    if (idle) last_idle = now;
    /* if not idle, we only flush if there's evidence that the app never goes idle */
    else if ((int)(now - last_idle) < 50) goto done;

    LIST_FOR_EACH_ENTRY( surface, &window_surfaces, struct window_surface, entry )
        surface->funcs->flush( surface );
done:
    pthread_mutex_unlock( &surfaces_lock );
}

/***********************************************************************
 *           NtUserGetProp   (win32u.@)
 *
 * NOTE Native allows only ATOMs as the second argument. We allow strings
 *      to save extra server call in GetPropW.
 */
HANDLE WINAPI NtUserGetProp( HWND hwnd, const WCHAR *str )
{
    ULONG_PTR ret = 0;

    SERVER_START_REQ( get_window_property )
    {
        req->window = wine_server_user_handle( hwnd );
        if (IS_INTRESOURCE(str)) req->atom = LOWORD(str);
        else wine_server_add_data( req, str, lstrlenW(str) * sizeof(WCHAR) );
        if (!wine_server_call_err( req )) ret = reply->data;
    }
    SERVER_END_REQ;
    return (HANDLE)ret;
}

/*****************************************************************************
 *           NtUserSetProp    (win32u.@)
 *
 * NOTE Native allows only ATOMs as the second argument. We allow strings
 *      to save extra server call in SetPropW.
 */
BOOL WINAPI NtUserSetProp( HWND hwnd, const WCHAR *str, HANDLE handle )
{
    BOOL ret;

    SERVER_START_REQ( set_window_property )
    {
        req->window = wine_server_user_handle( hwnd );
        req->data   = (ULONG_PTR)handle;
        if (IS_INTRESOURCE(str)) req->atom = LOWORD(str);
        else wine_server_add_data( req, str, lstrlenW(str) * sizeof(WCHAR) );
        ret = !wine_server_call_err( req );
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *           NtUserRemoveProp   (win32u.@)
 *
 * NOTE Native allows only ATOMs as the second argument. We allow strings
 *      to save extra server call in RemovePropW.
 */
HANDLE WINAPI NtUserRemoveProp( HWND hwnd, const WCHAR *str )
{
    ULONG_PTR ret = 0;

    SERVER_START_REQ( remove_window_property )
    {
        req->window = wine_server_user_handle( hwnd );
        if (IS_INTRESOURCE(str)) req->atom = LOWORD(str);
        else wine_server_add_data( req, str, lstrlenW(str) * sizeof(WCHAR) );
        if (!wine_server_call_err( req )) ret = reply->data;
    }
    SERVER_END_REQ;

    return (HANDLE)ret;
}


/*****************************************************************************
 *           NtUserGetLayeredWindowAttributes (win32u.@)
 */
BOOL WINAPI NtUserGetLayeredWindowAttributes( HWND hwnd, COLORREF *key, BYTE *alpha, DWORD *flags )
{
    BOOL ret;

    SERVER_START_REQ( get_window_layered_info )
    {
        req->handle = wine_server_user_handle( hwnd );
        if ((ret = !wine_server_call_err( req )))
        {
            if (key) *key = reply->color_key;
            if (alpha) *alpha = reply->alpha;
            if (flags) *flags = reply->flags;
        }
    }
    SERVER_END_REQ;

    return ret;
}

/*****************************************************************************
 *           NtUserBuildHwndList (win32u.@)
 */
NTSTATUS WINAPI NtUserBuildHwndList( HDESK desktop, ULONG unk2, ULONG unk3, ULONG unk4,
                                     ULONG thread_id, ULONG count, HWND *buffer, ULONG *size )
{
    user_handle_t *list = (user_handle_t *)buffer;
    int i;
    NTSTATUS status;

    SERVER_START_REQ( get_window_children )
    {
        req->desktop = wine_server_obj_handle( desktop );
        req->tid = thread_id;
        if (count) wine_server_set_reply( req, list, (count - 1) * sizeof(user_handle_t) );
        status = wine_server_call( req );
        if (status && status != STATUS_BUFFER_TOO_SMALL) return status;
        *size = reply->count + 1;
    }
    SERVER_END_REQ;
    if (*size > count) return STATUS_BUFFER_TOO_SMALL;

    /* start from the end since HWND is potentially larger than user_handle_t */
    for (i = *size - 2; i >= 0; i--)
        buffer[i] = wine_server_ptr_handle( list[i] );
    buffer[*size - 1] = HWND_BOTTOM;
    return STATUS_SUCCESS;
}
