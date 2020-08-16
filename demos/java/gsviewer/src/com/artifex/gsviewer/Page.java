package com.artifex.gsviewer;

import java.awt.Dimension;
import java.awt.image.BufferedImage;
import java.util.Collection;
import java.util.HashSet;

import com.artifex.gsviewer.ImageUtil.ImageParams;

/**
 * A Page represents an individual page within a Document. It stores a high resolution image
 * and a low resolution preview image.
 *
 * @author Ethan Vrhel
 *
 */
public class Page {

	/**
	 * The high-resolution DPI to use.
	 */
	public static final int PAGE_HIGH_DPI = 72;

	public static final String PAGE_HIGH_DPI_STR = toDPIString(PAGE_HIGH_DPI);

	/**
	 * The low-resolution DPI to use.
	 */
	public static final int PAGE_LOW_DPI = 10;

	public static final String PAGE_LOW_DPI_STR = toDPIString(PAGE_LOW_DPI);

	public static String toDPIString(int dpi) {
		return "[" + dpi + " " + dpi + "]";
	}

	private volatile BufferedImage lowRes;
	private volatile BufferedImage highRes;
	private volatile BufferedImage zoomed;

	private Collection<PageUpdateCallback> callbacks;

	public Page() {
		this(null, null);
	}

	public Page(final BufferedImage lowRes) {
		this(lowRes, null);
	}

	public Page(final BufferedImage lowRes, final BufferedImage highRes) {
		this.lowRes = lowRes;
		this.highRes = highRes;
		this.callbacks = new HashSet<>();
	}

	public void loadHighRes(final byte[] data, final int width, final int height, final int raster, final int format) {
		setHighRes(ImageUtil.createImage(data, new ImageParams(width, height, raster, format)));
	}

	public void setHighRes(final BufferedImage highRes) {
		unloadHighRes();
		this.highRes = highRes;
		for (PageUpdateCallback cb : callbacks) {
			cb.onLoadHighRes();
			cb.onPageUpdate();
		}
	}

	public void unloadHighRes() {
		if (highRes != null) {
			highRes.flush();
			highRes = null;
			for (PageUpdateCallback cb : callbacks) {
				cb.onUnloadHighRes();
				cb.onPageUpdate();
			}
		}
	}

	public void loadLowRes(final byte[] data, final int width, final int height, final int raster, final int format) {
		setLowRes(ImageUtil.createImage(data, new ImageParams(width, height, raster, format)));
	}

	public void setLowRes(final BufferedImage lowRes) {
		unloadLowRes();
		this.lowRes = lowRes;
		for (PageUpdateCallback cb : callbacks) {
			cb.onLoadLowRes();
			cb.onPageUpdate();
		}
	}

	public void unloadLowRes() {
		if (lowRes != null) {
			lowRes.flush();
			lowRes = null;
			for (PageUpdateCallback cb : callbacks) {
				cb.onUnloadLowRes();
				cb.onPageUpdate();
			}
		}
	}

	public void loadZoomed(final byte[] data, final int width, final int height,
			final int raster, final int format) {
		setZoomed(ImageUtil.createImage(data, new ImageParams(width, height, raster, format)));
	}

	public void setZoomed(final BufferedImage zoomed) {
		unloadZoomed();
		this.zoomed = zoomed;
		for (PageUpdateCallback cb : callbacks) {
			cb.onLoadZoomed();
			cb.onPageUpdate();
		}
	}

	public void unloadZoomed() {
		if (zoomed != null) {
			zoomed.flush();
			zoomed = null;
			for (PageUpdateCallback cb : callbacks) {
				cb.onUnloadZoomed();
				cb.onPageUpdate();
			}
		}
	}

	public void unloadAll() {
		unloadLowRes();
		unloadHighRes();
		unloadZoomed();
	}

	public BufferedImage getLowResImage() {
		return lowRes;
	}

	public BufferedImage getHighResImage() {
		return highRes;
	}

	public BufferedImage getZoomedImage() {
		return zoomed;
	}

	public BufferedImage getDisplayableImage() {
		return highRes == null ? lowRes : highRes;
	}

	public Dimension getLowResSize() {
		return lowRes == null ? null : new Dimension(lowRes.getWidth(), lowRes.getHeight());
	}

	public Dimension getHighResSize() {
		return highRes == null ? null : new Dimension(highRes.getWidth(), highRes.getHeight());
	}

	public Dimension getZoomedSize() {
		return zoomed == null ? null : new Dimension(zoomed.getWidth(), zoomed.getHeight());
	}

	public Dimension getDisplayableSize() {
		return highRes == null ? getLowResSize() : getHighResSize();
	}

	public Dimension getSize() {
		Dimension size = getHighResSize();
		if (size != null)
			return size;
		size = getLowResSize();
		if (size == null)
			return new Dimension(0, 0);
		return new Dimension(size.width * PAGE_HIGH_DPI / PAGE_LOW_DPI,
				size.height * PAGE_HIGH_DPI / PAGE_LOW_DPI);
	}

	public void addCallback(PageUpdateCallback cb) {
		callbacks.add(cb);
		cb.onPageUpdate();
	}

	public void removeCallback(PageUpdateCallback cb) {
		callbacks.remove(cb);
	}

	@Override
	public String toString() {
		return "Page[lowResLoaded=" + (lowRes != null) + ",highResLoaded=" + (highRes != null) + "]";
	}

	@Override
	public boolean equals(Object o) {
		if (o == this)
			return true;
		if (o instanceof Page) {
			Page p = (Page)o;
			boolean first = lowRes == null ? p.lowRes == null : lowRes.equals(p.lowRes);
			boolean second = highRes == null ? p.highRes == null : highRes.equals(p.highRes);
			return first && second;
		}
		return false;
	}

	@Override
	public int hashCode() {
		return (lowRes == null ? 0 : lowRes.hashCode()) + (highRes == null ? 0 : highRes.hashCode());
	}

	@Override
	public void finalize() {
		unloadAll();
	}
}
