/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * media_object.cpp - Media device objects: entities, pads and links
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include <linux/media.h>

#include "log.h"
#include "media_object.h"

/**
 * \file media_object.h
 * \brief Provides a class hierarchy that represents the  media objects exposed
 * by the Linux kernel Media Controller APIs.
 *
 * The abstract MediaObject class represents any Media Controller graph object
 * identified by an id unique in the media device context. It is subclassed by
 * the MediaEntity, MediaPad and MediaLink classes that represent the entities,
 * pads and links respectively. They are populated based on the media graph
 * information exposed by the Linux kernel through the MEDIA_IOC_G_TOPOLOGY
 * ioctl.
 *
 * As the media objects represent their kernel counterpart, information about
 * the properties they expose can be found in the Linux kernel documentation.
 *
 * All media objects are meant to be created and destroyed solely by the
 * MediaDevice and thus have private constructors and destructors.
 */

namespace libcamera {

/**
 * \class MediaObject
 * \brief Base class for all media objects
 *
 * MediaObject is an abstract base class for all media objects in the media
 * graph. Every media graph object is identified by an id unique in the media
 * device context, and this base class provides that.
 *
 * \sa MediaEntity, MediaPad, MediaLink
 */

/**
 * \fn MediaObject::MediaObject()
 * \brief Construct a MediaObject with \a id
 * \param id The media object id
 *
 * The caller shall ensure unicity of the object id in the media device context.
 * This constraint is neither enforced nor checked by the MediaObject.
 */

/**
 * \fn MediaObject::id()
 * \brief Retrieve the media object id
 * \return The media object id
 */

/**
 * \var MediaObject::id_
 * \brief The media object id
 */

/**
 * \class MediaLink
 * \brief The MediaLink represents a link between two pads in the media graph.
 *
 * Links are created from the information provided by the Media Controller API
 * in the media_v2_link structure. They reference the source() and sink() pads
 * they connect and track the link status through link flags().
 *
 * Each link is referenced in the link array of both of the pads it connect.
 */

/**
 * \brief Construct a MediaLink
 * \param link The media link kernel data
 * \param source The source pad at the origin of the link
 * \param sink The sink pad at the destination of the link
 */
MediaLink::MediaLink(const struct media_v2_link *link, MediaPad *source,
		     MediaPad *sink)
	: MediaObject(link->id), source_(source),
	  sink_(sink), flags_(link->flags)
{
}

/**
 * \fn MediaLink::source()
 * \brief Retrieve the link's source pad
 * \return The source pad at the origin of the link
 */

/**
 * \fn MediaLink::sink()
 * \brief Retrieve the link's sink pad
 * \return The sink pad at the destination of the link
 */

/**
 * \fn MediaLink::flags()
 * \brief Retrieve the link's flags
 *
 * Link flags are a bitmask of flags defined by the Media Controller API
 * MEDIA_LNK_FL_* macros.
 *
 * \return The link flags
 */

/**
 * \class MediaPad
 * \brief The MediaPad represents a pad of an entity in the media graph
 *
 * Pads are created from the information provided by the Media Controller API
 * in the media_v2_pad structure. They reference the entity() they belong to.
 *
 * In addition to its graph id, every media graph pad is identified by an index
 * unique in the context of the entity the pad belongs to.
 *
 * A pad can be either a 'source' pad or a 'sink' pad. This information is
 * captured in the pad flags().
 *
 * Pads are connected through links. Links originating from a source pad are
 * outbound links, and links arriving at a sink pad are inbound links. Pads
 * reference all the links() that are connected to them.
 */

/**
 * \brief Construct a MediaPad
 * \param pad The media pad kernel data
 * \param entity The entity the pad belongs to
 */
MediaPad::MediaPad(const struct media_v2_pad *pad, MediaEntity *entity)
	: MediaObject(pad->id), index_(pad->index), entity_(entity),
	  flags_(pad->flags)
{
}

MediaPad::~MediaPad()
{
	/*
	 * Don't delete the links as we only borrow the reference owned by
	 * MediaDevice.
	 */
	links_.clear();
}

/**
 * \fn MediaPad::index()
 * \brief Retrieve the pad index
 * \return The 0-based pad index identifying the pad in the context of the
 * entity it belongs to
 */

/**
 * \fn MediaPad::entity()
 * \brief Retrieve the entity the pad belongs to
 * \return The MediaEntity the pad belongs to
 */

/**
 * \fn MediaPad::flags()
 * \brief Retrieve the pad flags
 *
 * Pad flags are a bitmask of flags defined by the Media Controller API
 * MEDIA_PAD_FL_* macros.
 *
 * \return The pad flags
 */

/**
 * \fn MediaPad::links()
 * \brief Retrieve all links in the pad
 * \return A list of links connected to the pad
 */

/**
 * \brief Add a new link to this pad
 * \param link The MediaLink to add
 */
void MediaPad::addLink(MediaLink *link)
{
	links_.push_back(link);
}

/**
 * \class MediaEntity
 * \brief The MediaEntity represents an entity in the media graph
 *
 * Entities are created from the information provided by the Media Controller
 * API in the media_v2_entity structure. They reference the pads() they contain.
 *
 * In addition to its graph id, every media graph entity is identified by a
 * name() unique in the media device context.
 *
 * \todo Add support for associating a devnode to the entity when integrating
 * with DeviceEnumerator.
 */

/**
 * \fn MediaEntity::name()
 * \brief Retrieve the entity name
 * \return The entity name
 */

/**
 * \fn MediaEntity::pads()
 * \brief Retrieve all pads of the entity
 * \return The list of the entity's pads
 */

/**
 * \brief Get a pad in this entity by its index
 * \param index The 0-based pad index
 * \return The pad identified by \a index, or nullptr if no such pad exist
 */
const MediaPad *MediaEntity::getPadByIndex(unsigned int index) const
{
	for (MediaPad *p : pads_) {
		if (p->index() == index)
			return p;
	}

	return nullptr;
}

/**
 * \brief Get a pad in this entity by its object id
 * \param id The pad id
 * \return The pad identified by \a id, or nullptr if no such pad exist
 */
const MediaPad *MediaEntity::getPadById(unsigned int id) const
{
	for (MediaPad *p : pads_) {
		if (p->id() == id)
			return p;
	}

	return nullptr;
}

/**
 * \brief Construct a MediaEntity
 * \param entity The media entity kernel data
 */
MediaEntity::MediaEntity(const struct media_v2_entity *entity)
	: MediaObject(entity->id), name_(entity->name)
{
}

MediaEntity::~MediaEntity()
{
	/*
	 * Don't delete the pads as we only borrow the reference owned by
	 * MediaDevice.
	 */
	pads_.clear();
}

/**
 * \brief Add \a pad to the entity's list of pads
 * \param pad The pad to add to the list
 *
 * This function is meant to add pads to the entity during parsing of the media
 * graph, after the MediaPad objects are constructed and before the MediaDevice
 * is made available externally.
 */
void MediaEntity::addPad(MediaPad *pad)
{
	pads_.push_back(pad);
}

} /* namespace libcamera */